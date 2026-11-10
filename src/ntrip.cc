// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/ntrip.h"

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "internal/clock.h"
#include "internal/net.h"
#include "lgrtk/framer.h"
#include "lgrtk/version.h"

namespace lgrtk {

namespace {

using internal::kNsPerMs;
using internal::MonotonicNs;
using internal::MsToNs;

std::string UserAgent() { return std::string("NTRIP lgrtk/") + kVersion; }

void AppendDegMin(std::string& s, double deg, int deg_digits, char pos,
                  char neg) {
  const char hemi = deg < 0 ? neg : pos;
  deg = std::fabs(deg);
  int d = static_cast<int>(deg);
  double m = (deg - d) * 60.0;
  if (std::lround(m * 1e7) >= 60 * 10'000'000L) {  // rounding up to 60.0000000
    ++d;
    m = 0.0;
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "%0*d%010.7f,%c", deg_digits, d, m, hemi);
  s += buf;
}

}  // namespace

const char* ToString(NtripState s) noexcept {
  switch (s) {
    case NtripState::kIdle:
      return "idle";
    case NtripState::kConnecting:
      return "connecting";
    case NtripState::kStreaming:
      return "streaming";
    case NtripState::kStalled:
      return "stalled";
    case NtripState::kBackoff:
      return "backoff";
    case NtripState::kAuthFailed:
      return "auth-failed";
    case NtripState::kNoPosition:
      return "no-position";
    case NtripState::kStopped:
      return "stopped";
  }
  return "?";
}

std::string Base64(std::string_view in) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                       (static_cast<uint8_t>(in[i + 1]) << 8) |
                       static_cast<uint8_t>(in[i + 2]);
    out += kTable[(v >> 18) & 63];
    out += kTable[(v >> 12) & 63];
    out += kTable[(v >> 6) & 63];
    out += kTable[v & 63];
  }
  if (i + 1 == in.size()) {
    const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
    out += kTable[(v >> 18) & 63];
    out += kTable[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) |
                       (static_cast<uint8_t>(in[i + 1]) << 8);
    out += kTable[(v >> 18) & 63];
    out += kTable[(v >> 12) & 63];
    out += kTable[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

std::string BuildGga(const Fix& f) {
  const double utc = std::fmod(std::max(f.utc_s, 0.0), 86400.0);
  const int hh = static_cast<int>(utc / 3600);
  const int mm = static_cast<int>(std::fmod(utc, 3600.0) / 60);
  const double ss = std::fmod(utc, 60.0);
  char buf[64];
  std::string body = "GPGGA,";
  std::snprintf(buf, sizeof buf, "%02d%02d%05.2f,", hh, mm, ss);
  body += buf;
  AppendDegMin(body, f.lat_deg, 2, 'N', 'S');
  body += ',';
  AppendDegMin(body, f.lon_deg, 3, 'E', 'W');
  int q = ToGgaQuality(f.type);
  // A caster only needs a usable position: the other solution kinds (PPS,
  // dead reckoning, manual, simulation, none) are reported as autonomous.
  if (q != 1 && q != 2 && q != 4 && q != 5) {
    q = f.has_position ? 1 : 0;
  }
  std::snprintf(buf, sizeof buf, ",%d,%02d,%.1f,%.3f,M,%.3f,M,,", q, f.sats,
                static_cast<double>(f.hdop), f.h_ell_m - f.geoid_sep_m,
                f.geoid_sep_m);
  body += buf;
  unsigned sum = 0;
  for (char c : body) {
    sum ^= static_cast<unsigned char>(c);
  }
  std::snprintf(buf, sizeof buf, "*%02X\r\n", sum & 0xFF);
  return "$" + body + buf;
}

NtripClient::NtripClient(NtripOptions options, CorrectionSink& sink)
    : opt_(std::move(options)), sink_(sink) {}

NtripClient::~NtripClient() { Stop(); }

void NtripClient::SetPosition(const Fix& fix) {
  if (!fix.has_position) {
    return;
  }
  std::lock_guard lk(mu_);
  position_ = fix;
}

bool NtripClient::Start() {
  if (running_.exchange(true)) {
    return false;
  }
  stop_ = false;
  thread_ = std::thread([this] { Run(); });
  return true;
}

void NtripClient::Stop() {
  if (!running_) {
    return;
  }
  stop_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  running_ = false;
}

NtripStatus NtripClient::Status() const {
  std::lock_guard lk(mu_);
  NtripStatus s = status_;
  s.last_data_age_ns = last_data_ns_ == 0 ? -1 : MonotonicNs() - last_data_ns_;
  return s;
}

void NtripClient::SetState(NtripState s, const std::string& reason) {
  NtripState from;
  {
    std::lock_guard lk(mu_);
    from = status_.state;
    if (s == NtripState::kAuthFailed || s == NtripState::kBackoff ||
        s == NtripState::kStalled) {
      if (!reason.empty()) {
        status_.last_error = reason;
      }
    }
    if (from == s) {
      return;
    }
    status_.state = s;
  }
  if (on_status_) {
    on_status_(from, s, reason);
  }
}

bool NtripClient::BackoffWait() {
  const int64_t until = MonotonicNs() + MsToNs(backoff_ms_);
  while (!StopRequested() && MonotonicNs() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  backoff_ms_ = std::min(backoff_ms_ * 2, opt_.backoff_max_ms);
  return !StopRequested();
}

void NtripClient::Run() {
  backoff_ms_ = opt_.backoff_min_ms;
  while (!StopRequested()) {
    if (opt_.send_gga) {
      bool have = false;
      while (!StopRequested()) {
        {
          std::lock_guard lk(mu_);
          have = position_.has_value();
        }
        if (have) {
          break;
        }
        SetState(NtripState::kNoPosition,
                 "waiting for a rover position to send as GGA");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      if (!have) {
        break;
      }
    }
    SetState(
        NtripState::kConnecting,
        opt_.host + ":" + std::to_string(opt_.port) + "/" + opt_.mountpoint);
    Session();
    NtripState st;
    {
      std::lock_guard lk(mu_);
      st = status_.state;
    }
    if (st == NtripState::kAuthFailed) {
      while (!StopRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      break;
    }
    if (StopRequested()) {
      break;
    }
    std::string err;
    {
      std::lock_guard lk(mu_);
      err = status_.last_error;
    }
    SetState(NtripState::kBackoff, err);
    if (!BackoffWait()) {
      break;
    }
  }
  SetState(NtripState::kStopped, "stopped");
}

void NtripClient::Session() {
  auto fail = [&](const std::string& why) {
    std::lock_guard lk(mu_);
    status_.last_error = why;
  };
  std::string err;
  internal::UniqueFd sock = internal::Connect(
      opt_.host, opt_.port, opt_.connect_timeout_ms, err, &stop_);
  if (!sock) {
    fail(err);
    return;
  }
  std::optional<Fix> pos;
  {
    std::lock_guard lk(mu_);
    pos = position_;
  }
  const std::string gga = opt_.send_gga && pos ? BuildGga(*pos) : std::string();
  std::string req = "GET /" + opt_.mountpoint +
                    (opt_.version == 1 ? " HTTP/1.0\r\n" : " HTTP/1.1\r\n");
  req += "User-Agent: " + UserAgent() + "\r\n";
  if (opt_.version != 1) {
    req += "Host: " + opt_.host + ":" + std::to_string(opt_.port) + "\r\n";
    req += "Ntrip-Version: Ntrip/2.0\r\n";
    if (!gga.empty()) {
      req += "Ntrip-GGA: " + gga.substr(0, gga.size() - 2) + "\r\n";
    }
    req += "Connection: close\r\n";
  }
  if (!opt_.user.empty()) {
    req += "Authorization: Basic " + Base64(opt_.user + ":" + opt_.password) +
           "\r\n";
  }
  req += "\r\n";
  if (!internal::SendAll(sock.Get(), req)) {
    fail("cannot send request to " + opt_.host);
    return;
  }

  // Response header: "ICY 200 OK\r\n" (v1) or an HTTP header ending in a blank
  // line.
  std::string head;
  std::array<char, 4096> buf{};
  const int64_t deadline = MonotonicNs() + MsToNs(opt_.connect_timeout_ms);
  size_t header_end = std::string::npos;
  bool icy = false;
  while (!StopRequested()) {
    if (head.starts_with("ICY 200 OK\r\n")) {
      icy = true;
      header_end = 12;
      break;
    }
    header_end = head.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      header_end += 4;
      break;
    }
    if (MonotonicNs() > deadline) {
      fail("no response header from " + opt_.host);
      return;
    }
    pollfd p{sock.Get(), POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) {
      continue;
    }
    const ssize_t n = ::recv(sock.Get(), buf.data(), buf.size(), 0);
    if (n <= 0) {
      fail(head.empty() ? "caster closed the connection"
                        : "truncated response: " + head.substr(0, 40));
      return;
    }
    head.append(buf.data(), static_cast<size_t>(n));
  }
  if (StopRequested()) {
    return;
  }

  bool chunked = false;
  int http = 0;
  if (!icy) {
    const std::string status_line = head.substr(0, head.find("\r\n"));
    if (status_line.starts_with("SOURCETABLE")) {
      fail("mountpoint '" + opt_.mountpoint +
           "' not found (the caster answered with its sourcetable)");
      return;
    }
    if (std::sscanf(status_line.c_str(), "HTTP/%*d.%*d %d", &http) != 1) {
      fail("unexpected response: " + status_line);
      return;
    }
    {
      std::lock_guard lk(mu_);
      status_.http_status = http;
    }
    if (http == 401) {
      {
        std::lock_guard lk(mu_);
        status_.last_error = "HTTP 401 Unauthorized for mountpoint '" +
                             opt_.mountpoint +
                             "': check user name and password";
      }
      SetState(NtripState::kAuthFailed, "HTTP 401 Unauthorized");
      return;
    }
    if (http != 200) {
      fail("HTTP " + std::to_string(http) + " from caster: " + status_line);
      return;
    }
    chunked = internal::Lower(head.substr(0, header_end))
                  .find("transfer-encoding: chunked") != std::string::npos;
  }
  if (opt_.version == 1 && !gga.empty() &&
      !internal::SendAll(sock.Get(), gga)) {
    fail("cannot send GGA");
    return;
  }

  {
    std::lock_guard lk(mu_);
    ++status_.connects;
    last_data_ns_ = MonotonicNs();
  }
  SetState(NtripState::kStreaming, "connected to " + opt_.mountpoint);

  StreamFramer framer;
  internal::Dechunker dechunk;
  auto forward = [&](std::string_view data) {
    framer.Feed(
        std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()),
        [&](const Frame& f) {
          if (f.kind != FrameKind::kRtcm) {
            return;
          }
          sink_.Inject(f.bytes);
          std::lock_guard lk(mu_);
          ++status_.frames;
        });
  };
  auto consume = [&](std::string_view data) {
    {
      std::lock_guard lk(mu_);
      status_.bytes += data.size();
      last_data_ns_ = MonotonicNs();
    }
    backoff_ms_ = opt_.backoff_min_ms;
    if (!chunked) {
      forward(data);
      return true;
    }
    return dechunk.Feed(data, forward);
  };
  if (head.size() > header_end &&
      !consume(std::string_view(head).substr(header_end))) {
    fail("bad chunked encoding");
    return;
  }

  int64_t next_gga = MonotonicNs() + MsToNs(opt_.gga_interval_ms);
  while (!StopRequested()) {
    const int64_t now = MonotonicNs();
    if (opt_.send_gga && now >= next_gga) {
      next_gga = now + MsToNs(opt_.gga_interval_ms);
      {
        std::lock_guard lk(mu_);
        pos = position_;
      }
      if (pos && !internal::SendAll(sock.Get(), BuildGga(*pos))) {
        fail("connection lost while sending GGA");
        return;
      }
    }
    int64_t last;
    {
      std::lock_guard lk(mu_);
      last = last_data_ns_;
    }
    if (now - last > MsToNs(opt_.stall_after_ms)) {
      const std::string why =
          "no corrections for " + std::to_string(opt_.stall_after_ms) + " ms";
      SetState(NtripState::kStalled, why);
      return;
    }
    pollfd p{sock.Get(), POLLIN, 0};
    if (::poll(&p, 1, 50) <= 0) {
      continue;
    }
    const ssize_t n = ::recv(sock.Get(), buf.data(), buf.size(), 0);
    if (n <= 0) {
      fail("caster closed the connection");
      return;
    }
    if (!consume(std::string_view(buf.data(), static_cast<size_t>(n)))) {
      fail("bad chunked encoding");
      return;
    }
  }
}

std::optional<std::string> NtripClient::FetchSourcetable(
    const std::string& host, int port, int version, std::string& err) {
  internal::UniqueFd sock = internal::Connect(host, port, 5000, err);
  if (!sock) {
    return std::nullopt;
  }
  std::string req = version == 1 ? "GET / HTTP/1.0\r\n" : "GET / HTTP/1.1\r\n";
  req += "User-Agent: " + UserAgent() + "\r\n";
  if (version != 1) {
    req += "Host: " + host + ":" + std::to_string(port) +
           "\r\nNtrip-Version: Ntrip/2.0\r\n";
  }
  req += "Connection: close\r\n\r\n";
  if (!internal::SendAll(sock.Get(), req)) {
    err = "cannot send request";
    return std::nullopt;
  }
  std::string all;
  std::array<char, 4096> buf{};
  const int64_t deadline = MonotonicNs() + 10'000'000'000;
  while (MonotonicNs() < deadline &&
         all.find("ENDSOURCETABLE") == std::string::npos) {
    pollfd p{sock.Get(), POLLIN, 0};
    if (::poll(&p, 1, 100) <= 0) {
      continue;
    }
    const ssize_t n = ::recv(sock.Get(), buf.data(), buf.size(), 0);
    if (n <= 0) {
      break;
    }
    all.append(buf.data(), static_cast<size_t>(n));
  }
  const size_t end = all.find("\r\n\r\n");
  if (end == std::string::npos) {
    err = "no sourcetable from " + host;
    return std::nullopt;
  }
  std::string body = all.substr(end + 4);
  if (internal::Lower(all.substr(0, end)).find("transfer-encoding: chunked") !=
      std::string::npos) {
    std::string plain;
    internal::Dechunker d;
    d.Feed(body, [&](std::string_view s) { plain += s; });
    body = plain;
  }
  return body;
}

}  // namespace lgrtk
