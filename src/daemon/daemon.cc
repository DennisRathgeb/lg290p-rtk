// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/daemon.h"

#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <utility>
#include <vector>

#include "daemon/gpsd_json.h"
#include "daemon/sd_notify.h"
#include "lgrtk/lg290p.h"
#include "lgrtk/version.h"

namespace lgrtk::daemon {
namespace {

int64_t ClockNs(clockid_t id) {
  timespec ts{};
  clock_gettime(id, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

void Log(const std::string& msg) { std::fprintf(stderr, "%s\n", msg.c_str()); }

bool Active(State s) { return s == State::kStreaming || s == State::kStale; }

}  // namespace

Daemon::Daemon(DaemonConfig config) : cfg_(std::move(config)) {}

Daemon::~Daemon() { Stop(); }

bool Daemon::Start(std::string* err) {
  if (!loop_.Init(err)) {
    return false;
  }
  if (cfg_.ntrip.enabled) {
    auto pw = ReadNtripPassword(cfg_.ntrip, err);
    if (!pw) {
      return false;
    }
    ntrip_password_ = std::move(*pw);
  }
  std::string host;
  int port = 0;
  if (!ParseHostPort(cfg_.gpsd_listen, &host, &port)) {
    *err = "bad gpsd.listen " + cfg_.gpsd_listen;
    return false;
  }
  gpsd_ = std::make_unique<GpsdServer>(&loop_, cfg_.device_name);
  if (!gpsd_->Listen(host, port, err)) {
    return false;
  }
  if (!cfg_.raw_listen.empty()) {
    if (!ParseHostPort(cfg_.raw_listen, &host, &port)) {
      *err = "bad raw.listen " + cfg_.raw_listen;
      return false;
    }
    raw_ = std::make_unique<RawTap>(&loop_);
    if (!raw_->Listen(host, port, err)) {
      return false;
    }
  }
  if (!cfg_.chrony_sock.empty()) {
    chrony_ = std::make_unique<ChronySock>(cfg_.chrony_sock);
  }
  if (!cfg_.control_sock.empty()) {
    ControlHandlers h;
    h.status = [this] { return StatusJson(); };
    h.reset = [this](const std::string& kind) {
      auto cmd = lg290p::RestartByName(kind);
      if (!cmd) {  // ControlServer passes only known kinds
        return nlohmann::json{{kFieldError, "unknown reset kind " + kind}};
      }
      Log("control: reset " + kind);
      // Restarts do not answer; the receiver's state machine notices the
      // restart, so there is nothing to wait for here.
      (void)rx_->Send(std::move(*cmd));
      return nlohmann::json{{"sent", kind}};
    };
    h.apply_default = [this] {
      Log("control: apply-default (restarting the receiver)");
      StopDevices();
      cfg_.receiver.config = lg290p::DefaultRoverConfig();
      cfg_.receiver.apply_config = true;
      StartDevices();
      return nlohmann::json{
          {"applied",
           "default rover configuration; the receiver is being "
           "re-identified and configured"}};
    };
    control_ = std::make_unique<ControlServer>(&loop_, std::move(h));
    if (!control_->Listen(cfg_.control_sock, err)) {
      return false;
    }
  }
  StartDevices();
  const int64_t wd = WatchdogPeriodMs();
  if (wd > 0) {
    loop_.Every(wd, [] { SdNotify("WATCHDOG=1"); });
  }
  Log(std::string("lgrtkd ") + kVersion + ": gpsd on " + cfg_.gpsd_listen +
      " (port " + std::to_string(gpsd_->Port()) + "), receiver " +
      cfg_.receiver.port + (raw_ ? ", raw tap on " + cfg_.raw_listen : "") +
      (chrony_ ? ", chrony " + cfg_.chrony_sock : "") +
      (ntrip_ ? ", NTRIP " + cfg_.ntrip.host + "/" + cfg_.ntrip.mountpoint
              : ""));
  SdNotify("READY=1");
  UpdateSystemdStatus();
  return true;
}

void Daemon::StartDevices() {
  rx_ = std::make_unique<Lg290pReceiver>(cfg_.receiver);
  rx_->OnFix([this](const Fix& f) {
    // Arrival in system time: CLOCK_REALTIME now minus the time since the
    // epoch's last byte was read (CLOCK_MONOTONIC).
    const int64_t arrival =
        ClockNs(CLOCK_REALTIME) - (ClockNs(CLOCK_MONOTONIC) - f.t_arrival_ns);
    if (ntrip_) {
      ntrip_->SetPosition(f);
    }
    loop_.Post([this, f, arrival] { OnFix(f, arrival); });
  });
  rx_->OnNmea([this](std::string_view line) {
    loop_.Post([this, s = std::string(line)] {
      sky_.FeedNmea(s);
      gpsd_->PublishNmea(s);
    });
  });
  if (raw_) {
    rx_->OnRaw([this](std::span<const uint8_t> bytes) {
      loop_.Post([this, v = std::vector<uint8_t>(bytes.begin(), bytes.end())] {
        raw_->Publish(v);
      });
    });
  }
  rx_->OnStatus([this](const StatusEvent& ev) {
    loop_.Post([this, ev] { OnState(ev); });
  });
  // The NTRIP client exists before the receiver thread starts and outlives
  // it (StopDevices), because the receiver's OnFix reads ntrip_ on its own
  // thread.
  if (cfg_.ntrip.enabled) {
    NtripOptions o;
    o.host = cfg_.ntrip.host;
    o.port = cfg_.ntrip.port;
    o.mountpoint = cfg_.ntrip.mountpoint;
    o.user = cfg_.ntrip.user;
    o.password = ntrip_password_;
    o.version = cfg_.ntrip.version;
    o.send_gga = cfg_.ntrip.send_gga;
    ntrip_ = std::make_unique<NtripClient>(o, *rx_);
    ntrip_->OnStatus(
        [this](NtripState from, NtripState to, const std::string& why) {
          loop_.Post([from, to, why] {
            Log(std::string("ntrip: ") + ToString(from) + " -> " +
                ToString(to) + (why.empty() ? "" : " (" + why + ")"));
          });
        });
  }
  rx_->Start();
  if (ntrip_) {
    ntrip_->Start();
  }
}

void Daemon::StopDevices() {
  // Stop both threads before destroying either: the receiver thread reads
  // ntrip_ (OnFix), and NTRIP injects into *rx_.
  if (ntrip_) {
    ntrip_->Stop();
  }
  if (rx_) {
    rx_->Stop();
  }
  ntrip_.reset();
  rx_.reset();
}

void Daemon::Stop() {
  SdNotify("STOPPING=1");
  StopDevices();
}

void Daemon::OnFix(const Fix& fix, int64_t arrival_realtime_ns) {
  gpsd_->PublishFix(fix);
  gpsd_->PublishSky(SkyJson(sky_, cfg_.device_name, FixUnixNs(fix)));
  if (chrony_) {
    if (auto s = MakeChronySample(fix, arrival_realtime_ns)) {
      chrony_->Send(*s);
    }
  }
}

void Daemon::OnState(const StatusEvent& ev) {
  state_ = ev.to;
  Log(std::string("receiver: ") + ToString(ev.from) + " -> " + ToString(ev.to) +
      (ev.reason.empty() ? "" : " (" + ev.reason + ")"));
  const int baud = rx_ ? rx_->Status().baud : 0;
  gpsd_->SetActive(Active(ev.to), Active(ev.to) ? baud : 0);
  UpdateSystemdStatus();
}

void Daemon::UpdateSystemdStatus() const {
  SdNotify(std::string("STATUS=receiver ") + ToString(state_) + ", " +
           std::to_string(gpsd_->Clients()) + " gpsd client(s)");
}

nlohmann::json Daemon::StatusJson() const {
  nlohmann::json j;
  j["version"] = kVersion;
  if (rx_) {
    const ReceiverStatus s = rx_->Status();
    j["state"] = ToString(s.state);
    j["port"] = s.port;
    j["baud"] = s.baud;
    j["firmware"] = s.firmware;
    j["bytes"] = s.bytes;
    j["epochs"] = s.epochs;
    j["reconnects"] = s.reconnects;
    j["rtcm_injected"] = s.rtcm_injected;
    j["bad_nmea"] = s.framer.bad_nmea;
    j["last_fix_age_ms"] =
        s.last_fix_age_ns < 0 ? -1 : s.last_fix_age_ns / 1000000;
    j["last_error"] = s.last_error;
  }
  j["gpsd_clients"] = gpsd_->Clients();
  j["gpsd_dropped"] = gpsd_->Dropped();
  if (raw_) {
    j["raw_clients"] = raw_->Clients();
  }
  if (chrony_) {
    j["chrony_samples"] = chrony_->Sent();
  }
  if (ntrip_) {
    const NtripStatus n = ntrip_->Status();
    j["ntrip_state"] = ToString(n.state);
    j["ntrip_bytes"] = n.bytes;
    j["ntrip_connects"] = n.connects;
  }
  return j;
}

void Daemon::Run(bool handle_signals) {
  int sfd = -1;
  if (handle_signals) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd >= 0) {
      loop_.Add(sfd, POLLIN, [this, sfd](short) {
        signalfd_siginfo si{};
        while (read(sfd, &si, sizeof si) == sizeof si) {
          Log("lgrtkd: signal " + std::to_string(si.ssi_signo) +
              ", shutting down");
          loop_.Quit();
        }
      });
    }
  }
  loop_.Run();
  if (sfd >= 0) {
    loop_.Remove(sfd);
    close(sfd);
  }
}

}  // namespace lgrtk::daemon
