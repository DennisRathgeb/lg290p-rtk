// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "sim/sim_lg290p.h"

#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "lgrtk/framer.h"
#include "lgrtk/lg290p.h"

namespace lgrtk::test {

namespace {

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

speed_t SpeedOf(int baud) {
  switch (baud) {
    case 9600:
      return B9600;
    case 115200:
      return B115200;
    case 230400:
      return B230400;
    case 460800:
      return B460800;
    case 921600:
      return B921600;
    default:
      return B0;
  }
}

std::string Line(const std::string& body) {
  std::string s = lg290p::BuildCommand(body);
  return s;
}

}  // namespace

SimLg290p::SimLg290p(int baud) : baud_(baud) {
  char tmpl[] = "/tmp/lgrtk-sim-XXXXXX";
  dir_ = mkdtemp(tmpl);
  link_ = dir_ + "/ttyLG290P";
  settings_ = {{"PQTMCFGFIXRATE", "100"},
               {"PQTMCFGRTK", "1,1,120"},
               {"PQTMCFGNMEADP", "3,8,3,2,3,2"},
               {"PQTMCFGELETHD", "5.0"},
               {"PQTMCFGCNRTHD", "10.0"},
               {"PQTMCFGCNST", "1,1,1,1,1,1"},
               {"PQTMCFGRCVRMODE", "1"},
               {"MSG:GGA", "1"},
               {"MSG:RMC", "1"},
               {"MSG:GST", "0"},
               {"MSG:PQTMEPE", "0,2"},
               {"MSG:GLL", "1"},
               {"MSG:VTG", "1"}};
  OpenPty();
  thread_ = std::thread([this] { Loop(); });
}

SimLg290p::~SimLg290p() {
  stop_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  ClosePty();
  std::filesystem::remove_all(dir_);
}

void SimLg290p::OpenPty() {
  std::lock_guard lk(mu_);
  int m = -1;
  int s = -1;
  if (openpty(&m, &s, nullptr, nullptr, nullptr) != 0) {
    std::abort();
  }
  termios t{};
  tcgetattr(s, &t);
  cfmakeraw(&t);  // VMIN 1; a client opening the port sets VMIN 0 (see Loop)
  cfsetspeed(&t, B460800);
  tcsetattr(s, TCSANOW, &t);
  fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
  master_ = m;
  slave_keep_ = s;  // keeps the pty alive while no client has it open
  std::filesystem::remove(link_);
  std::filesystem::create_symlink(ttyname(s), link_);
  plugged_ = true;
}

void SimLg290p::ClosePty() {
  std::lock_guard lk(mu_);
  plugged_ = false;
  client_open_ = false;
  if (master_ >= 0) {
    close(master_);
  }
  if (slave_keep_ >= 0) {
    close(slave_keep_);
  }
  master_ = slave_keep_ = -1;
  std::error_code ec;
  std::filesystem::remove(link_, ec);
}

void SimLg290p::Unplug() { ClosePty(); }
void SimLg290p::Replug() { OpenPty(); }

void SimLg290p::SetFixInterval(int ms) {
  std::lock_guard lk(mu_);
  settings_["PQTMCFGFIXRATE"] = std::to_string(ms);
}

void SimLg290p::NakWrites(const std::string& command) {
  std::lock_guard lk(mu_);
  nak_writes_.insert(command);
}

void SimLg290p::Mute(const std::string& command) {
  std::lock_guard lk(mu_);
  mute_.insert(command);
}

void SimLg290p::Nak(const std::string& command) {
  std::lock_guard lk(mu_);
  nak_.insert(command);
}

std::vector<std::string> SimLg290p::Writes() const {
  std::lock_guard lk(mu_);
  return writes_;
}

void SimLg290p::ClearWrites() {
  std::lock_guard lk(mu_);
  writes_.clear();
}

std::vector<uint8_t> SimLg290p::RtcmReceived() const {
  std::lock_guard lk(mu_);
  return rtcm_;
}

bool SimLg290p::HostBaudMatches() const {
  if (slave_keep_ < 0) {
    return false;
  }
  termios t{};
  if (tcgetattr(slave_keep_, &t) != 0) {
    return false;
  }
  return cfgetospeed(&t) == SpeedOf(baud_);
}

void SimLg290p::Send(const std::string& line) {
  // mu_ held by caller.
  if (master_ < 0) {
    return;
  }
  if (!HostBaudMatches()) {
    static const char kNoise[] = "\x8f\x13\xfe\x02\xa5\x7f\xc3\x91";
    (void)!write(master_, kNoise, sizeof kNoise - 1);
    return;
  }
  (void)!write(master_, line.data(), line.size());
}

void SimLg290p::HandleCommand(const std::string& body) {
  // mu_ held by caller.
  const size_t comma = body.find(',');
  const std::string cmd = body.substr(0, comma);
  const std::string rest =
      comma == std::string::npos ? "" : body.substr(comma + 1);
  if (mute_.count(cmd)) {
    return;
  }
  if (nak_writes_.count(cmd) && rest.starts_with("W,")) {
    Send(Line(cmd + ",ERROR,1"));
    return;
  }
  if (nak_.count(cmd)) {
    Send(Line(cmd + ",ERROR,1"));
    return;
  }
  if (cmd == "PQTMVERNO") {
    Send(Line("PQTMVERNO,LG290P03AANR01A06S,2025/09/18,15:57:00"));
    return;
  }
  if (cmd == "PQTMSAVEPAR") {
    writes_.push_back(body);
    Send(Line("PQTMSAVEPAR,OK"));
    return;
  }
  if (cmd == "PQTMSRR" || cmd == "PQTMHOT" || cmd == "PQTMWARM" ||
      cmd == "PQTMCOLD") {
    writes_.push_back(body);
    ++resets_;
    silent_until_ns_ = NowNs() + 300'000'000;
    return;
  }
  if (rest.starts_with("W,")) {
    writes_.push_back(body);
    const std::string v = rest.substr(2);
    if (cmd == "PQTMCFGMSGRATE") {
      const size_t c = v.find(',');
      settings_["MSG:" + v.substr(0, c)] =
          c == std::string::npos ? "" : v.substr(c + 1);
    } else {
      settings_[cmd] = v;
    }
    Send(Line(cmd + ",OK"));
    return;
  }
  if (rest == "R" || rest.starts_with("R,")) {
    if (cmd == "PQTMCFGMSGRATE") {
      std::string msg = rest.substr(2);
      const size_t c = msg.find(',');
      if (c != std::string::npos) {
        msg = msg.substr(0, c);
      }
      const auto it = settings_.find("MSG:" + msg);
      Send(Line(cmd + ",OK," + msg + "," +
                (it == settings_.end() ? "0" : it->second)));
      return;
    }
    const auto it = settings_.find(cmd);
    if (it == settings_.end()) {
      Send(Line(cmd + ",ERROR,3"));
    } else {
      Send(Line(cmd + ",OK," + it->second));
    }
    return;
  }
  Send(Line(cmd + ",ERROR,3"));
}

void SimLg290p::Loop() {
  StreamFramer framer;
  std::array<uint8_t, 1024> buf{};
  double utc = 10 * 3600.0;
  int64_t next_epoch = NowNs();
  while (!stop_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::lock_guard lk(mu_);
    if (!plugged_ || master_ < 0) {
      continue;
    }
    if (!client_open_) {
      termios t{};
      if (tcgetattr(slave_keep_, &t) == 0 && t.c_cc[VMIN] == 0) {
        client_open_ = true;
        if (burst_on_open_) {
          // An old epoch, its tail spliced into the next line, like the bench
          // capture.
          const std::string old =
              Line(
                  "GNGGA,003538.300,4637.80000000,N,00846.80000000,E,1,24,0.70,"
                  "1950.000,M,49.100,M,,") +
              Line(
                  "GNRMC,003538.300,A,4637.80000000,N,00846.80000000,E,0.010,"
                  "045.00,101126,,,A,V");
          (void)!write(master_, old.data(), old.size());
        }
      }
    }
    const ssize_t n = reading_ ? read(master_, buf.data(), buf.size()) : 0;
    const bool booting = NowNs() < silent_until_ns_;
    if (n > 0 && !booting && HostBaudMatches()) {
      framer.Feed(std::span(buf.data(), static_cast<size_t>(n)),
                  [&](const Frame& f) {
                    if (f.kind == FrameKind::kRtcm) {
                      rtcm_.insert(rtcm_.end(), f.bytes.begin(), f.bytes.end());
                    } else {
                      const std::string_view t = f.Text();
                      HandleCommand(std::string(t.substr(1, t.size() - 4)));
                    }
                  });
    }
    const int64_t now = NowNs();
    if (now < next_epoch) {
      continue;
    }
    int interval = std::atoi(settings_["PQTMCFGFIXRATE"].c_str());
    if (interval <= 0) {
      interval = 100;
    }
    next_epoch += int64_t{interval} * 1'000'000;
    if (next_epoch < now) {
      next_epoch = now;
    }
    utc += interval / 1000.0;
    if (silent_ || booting) {
      continue;
    }
    const int hh = static_cast<int>(utc / 3600);
    const int mm = static_cast<int>(std::fmod(utc, 3600) / 60);
    const double ss = std::fmod(utc, 60.0);
    char t[16];
    std::snprintf(t, sizeof t, "%02d%02d%06.3f", hh, mm, ss);
    const int q = quality_;
    const std::string age = (q == 4 || q == 5) ? "1.0,0042" : ",";
    auto rate = [&](const std::string& m) {
      return std::atoi(settings_["MSG:" + m].c_str());
    };
    if (rate("GGA") > 0) {
      Send(Line(std::string("GNGGA,") + t +
                ",4637.80000000,N,00846.80000000,E," + std::to_string(q) +
                ",24,0.70,1950.000,M,49.100,M," + age));
    }
    if (rate("RMC") > 0) {
      Send(Line(
          std::string("GNRMC,") + t +
          ",A,4637.80000000,N,00846.80000000,E,0.010,045.00,101126,,,A,V"));
    }
    if (rate("GST") > 0) {
      Send(Line(std::string("GNGST,") + t +
                ",0.400,0.030,0.020,30.0,0.021,0.018,0.045"));
    }
    if (rate("PQTMEPE") > 0) {
      Send(Line("PQTMEPE,2,0.020,0.020,0.050,0.028,0.057"));
    }
  }
}

}  // namespace lgrtk::test
