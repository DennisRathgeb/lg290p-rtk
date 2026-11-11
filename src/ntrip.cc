// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/ntrip.h"

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>

#include "internal/clock.h"
#include "internal/net.h"
#include "internal/text.h"
#include "lgrtk/framer.h"
#include "lgrtk/version.h"

namespace lgrtk {

namespace {

using internal::MonotonicNs;
using internal::MsToNs;

// Receive buffer for the caster connection.
constexpr size_t kRecvBufferSize = 4096;
// Bytes before the first RTCM frame searched for a status line from a proxy.
constexpr size_t kMaxStreamStatusBytes = 512;
// FetchSourcetable: TCP connect, then the whole table.
constexpr int kSourcetableConnectTimeoutMs = 5000;
constexpr int kSourcetableReadTimeoutMs = 10000;
// Sleep of the client thread while it waits (backoff, no position, 401).
constexpr int kIdleWaitMs = 20;
// How much of a malformed response goes into an error message.
constexpr size_t kResponseExcerpt = 40;
// NTRIP 1 casters answer a mountpoint request with this line alone.
constexpr std::string_view kIcyOk = "ICY 200 OK\r\n";
constexpr std::string_view kHeaderEnd = "\r\n\r\n";
constexpr int kHttpOk = 200;
constexpr int kHttpUnauthorized = 401;

std::string UserAgent() { return std::string("NTRIP lgrtk/") + kVersion; }

// One NTRIP request (a mountpoint or the sourcetable).
struct HttpRequest {
  std::string_view host;
  int port;
  int version;            // NTRIP 1 or 2
  std::string path;       // "/MOUNT", or "/" for the sourcetable
  std::string_view gga;   // "$GPGGA...\r\n" for NTRIP 2's header; may be empty
  std::string_view user;  // empty: no Authorization header
  std::string_view password;
  bool close_connection = false;  // ask the server to close after the reply
};

// The request text. NTRIP 1 is plain HTTP/1.0; NTRIP 2 adds Host,
// Ntrip-Version, the optional Ntrip-GGA header and Connection: close (also
// sent with NTRIP 1 when `close_connection` is set).
std::string BuildRequest(const HttpRequest& r) {
  std::string req =
      "GET " + r.path + (r.version == 1 ? " HTTP/1.0\r\n" : " HTTP/1.1\r\n");
  req += "User-Agent: " + UserAgent() + "\r\n";
  if (r.version != 1) {
    req += "Host: ";
    req += r.host;
    req += ":" + std::to_string(r.port) + "\r\n";
    req += "Ntrip-Version: Ntrip/2.0\r\n";
    if (!r.gga.empty()) {
      req += "Ntrip-GGA: ";
      req += internal::TrimLineEnd(r.gga);
      req += "\r\n";
    }
  }
  if (r.version != 1 || r.close_connection) {
    req += "Connection: close\r\n";
  }
  if (!r.user.empty()) {
    std::string credentials(r.user);
    credentials += ':';
    credentials += r.password;
    req += "Authorization: Basic " + Base64(credentials) + "\r\n";
  }
  req += "\r\n";
  return req;
}

// The caster's answer up to the end of its header.
struct HttpResponse {
  bool icy = false;         // NTRIP 1 "ICY 200 OK" (no header fields)
  std::string status_line;  // first line without CRLF
  int status = 0;           // HTTP status; 0 for ICY or an unparsable line
  bool chunked = false;     // Transfer-Encoding: chunked
  std::string body;         // bytes received after the header
};

// Reads until the response header is complete. nullopt with a reason in
// `err` on timeout, a closed connection or a stop request.
std::optional<HttpResponse> ReadResponseHeader(int fd, int64_t deadline_ns,
                                               const std::string& host,
                                               const std::atomic<bool>* stop,
                                               std::string& err) {
  std::string head;
  std::array<char, kRecvBufferSize> buf{};
  HttpResponse r;
  size_t header_end = std::string::npos;
  while (stop == nullptr || !stop->load(std::memory_order_relaxed)) {
    if (head.starts_with(kIcyOk)) {
      r.icy = true;
      header_end = kIcyOk.size();
      break;
    }
    header_end = head.find(kHeaderEnd);
    if (header_end != std::string::npos) {
      header_end += kHeaderEnd.size();
      break;
    }
    if (MonotonicNs() > deadline_ns) {
      err = "no response header from " + host;
      return std::nullopt;
    }
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, internal::kPollSliceMs) <= 0) {
      continue;
    }
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n <= 0) {
      err = head.empty()
                ? "caster closed the connection"
                : "truncated response: " + head.substr(0, kResponseExcerpt);
      return std::nullopt;
    }
    head.append(buf.data(), static_cast<size_t>(n));
  }
  if (header_end == std::string::npos) {
    err = "stopped";
    return std::nullopt;
  }
  r.body = head.substr(header_end);
  if (!r.icy) {
    r.status_line = head.substr(0, head.find("\r\n"));
    if (!internal::ParseHttpStatus(r.status_line, r.status)) {
      r.status = 0;
    }
    r.chunked = internal::Lower(head.substr(0, header_end))
                    .find("transfer-encoding: chunked") != std::string::npos;
  }
  return r;
}

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
  std::snprintf(buf, sizeof buf, "*%02X\r\n", internal::NmeaXor(body));
  return "$" + body + buf;
}

NtripClient::NtripClient(NtripOptions options, CorrectionSink& sink)
    : opt_(std::move(options)), sink_(sink) {}

NtripClient::~NtripClient() { Stop(); }

void NtripClient::SetPosition(const Fix& fix) {
  if (!fix.has_position) {
    return;
  }
  std::scoped_lock lk(mu_);
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
  std::scoped_lock lk(mu_);
  NtripStatus s = status_;
  s.last_data_age_ns = last_data_ns_ == 0 ? -1 : MonotonicNs() - last_data_ns_;
  return s;
}

void NtripClient::SetState(NtripState s, const std::string& reason) {
  NtripState from;
  {
    std::scoped_lock lk(mu_);
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
    std::this_thread::sleep_for(std::chrono::milliseconds(kIdleWaitMs));
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
          std::scoped_lock lk(mu_);
          have = position_.has_value();
        }
        if (have) {
          break;
        }
        SetState(NtripState::kNoPosition,
                 "waiting for a rover position to send as GGA");
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdleWaitMs));
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
      std::scoped_lock lk(mu_);
      st = status_.state;
    }
    if (st == NtripState::kAuthFailed) {
      while (!StopRequested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdleWaitMs));
      }
      break;
    }
    if (StopRequested()) {
      break;
    }
    std::string err;
    {
      std::scoped_lock lk(mu_);
      err = status_.last_error;
    }
    SetState(NtripState::kBackoff, err);
    if (!BackoffWait()) {
      break;
    }
  }
  SetState(NtripState::kStopped, "stopped");
}

void NtripClient::Fail(const std::string& why) {
  std::scoped_lock lk(mu_);
  status_.last_error = why;
}

void NtripClient::Session() {
  std::string err;
  const internal::UniqueFd sock = internal::Connect(
      opt_.host, opt_.port, opt_.connect_timeout_ms, err, &stop_);
  if (!sock) {
    Fail(err);
    return;
  }
  std::optional<Fix> pos;
  {
    std::scoped_lock lk(mu_);
    pos = position_;
  }
  const std::string gga = opt_.send_gga && pos ? BuildGga(*pos) : std::string();
  const HttpRequest request{
      opt_.host, opt_.port, opt_.version, "/" + opt_.mountpoint,
      gga,       opt_.user, opt_.password};
  if (!internal::SendAll(sock.Get(), BuildRequest(request))) {
    Fail("cannot send request to " + opt_.host);
    return;
  }

  const auto response = ReadResponseHeader(
      sock.Get(), MonotonicNs() + MsToNs(opt_.connect_timeout_ms), opt_.host,
      &stop_, err);
  if (StopRequested()) {
    return;
  }
  if (!response) {
    Fail(err);
    return;
  }
  if (!response->icy) {
    const std::string& status_line = response->status_line;
    if (status_line.starts_with("SOURCETABLE")) {
      Fail("mountpoint '" + opt_.mountpoint +
           "' not found (the caster answered with its sourcetable)");
      return;
    }
    if (response->status == 0) {
      Fail("unexpected response: " + status_line);
      return;
    }
    {
      std::scoped_lock lk(mu_);
      status_.http_status = response->status;
    }
    if (response->status == kHttpUnauthorized) {
      AuthFailed();
      return;
    }
    if (response->status != kHttpOk) {
      Fail("HTTP " + std::to_string(response->status) +
           " from caster: " + status_line);
      return;
    }
  }
  // NTRIP 1 has no request header for the position: it follows the request.
  if (opt_.version == 1 && !gga.empty() &&
      !internal::SendAll(sock.Get(), gga)) {
    Fail("cannot send GGA");
    return;
  }

  {
    std::scoped_lock lk(mu_);
    ++status_.connects;
    last_data_ns_ = MonotonicNs();
  }
  SetState(NtripState::kStreaming, "connected to " + opt_.mountpoint);
  StreamLoop(sock.Get(), response->chunked, response->body);
}

void NtripClient::AuthFailed() {
  {
    std::scoped_lock lk(mu_);
    status_.http_status = kHttpUnauthorized;
  }
  // SetState records the reason as the last error.
  SetState(NtripState::kAuthFailed, "HTTP 401 Unauthorized for mountpoint '" +
                                        opt_.mountpoint +
                                        "': check user name and password");
}

void NtripClient::StreamLoop(int fd, bool chunked, std::string_view early) {
  StreamFramer framer;
  internal::Dechunker dechunk;
  // An NTRIP proxy (e.g. IQProxy) may answer 200 at once and pass the
  // caster's own answer on in the stream: until the first RTCM frame, a
  // payload that starts with an HTTP status line is that answer.
  bool seen_rtcm = false;
  std::string head;  // payload before the first RTCM frame, while it may be
                     // a status line
  std::string stream_error;  // set: end the session with this failure
  bool auth_failed = false;
  auto check_head = [&](std::string_view data) {
    if (seen_rtcm || head.size() >= kMaxStreamStatusBytes) {
      return;
    }
    if (head.empty()) {  // "ICY 200 OK\r\n\r\n": the blank line arrives here
      const size_t start = data.find_first_not_of("\r\n");
      if (start == std::string_view::npos) {
        return;
      }
      data.remove_prefix(start);
    }
    head.append(data.substr(
        0, std::min(data.size(), kMaxStreamStatusBytes - head.size())));
    constexpr std::string_view kHttp = "HTTP/";
    if (head.compare(0, std::min(head.size(), kHttp.size()), kHttp, 0,
                     std::min(head.size(), kHttp.size())) != 0) {
      head.resize(kMaxStreamStatusBytes);  // not a status line: stop looking
      return;
    }
    const size_t eol = head.find("\r\n");
    if (eol == std::string::npos) {
      return;
    }
    const std::string line = head.substr(0, eol);
    int status = 0;
    if (!internal::ParseHttpStatus(line, status)) {
      return;
    }
    if (status == kHttpUnauthorized) {
      auth_failed = true;
    } else if (status != kHttpOk) {
      {
        std::scoped_lock lk(mu_);
        status_.http_status = status;
      }
      stream_error = "HTTP " + std::to_string(status) +
                     " from caster in the stream: " + line;
    }
    head.resize(kMaxStreamStatusBytes);
  };
  auto forward = [&](std::string_view data) {
    check_head(data);
    framer.Feed(
        std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()),
        [&](const Frame& f) {
          if (f.kind != FrameKind::kRtcm) {
            return;
          }
          seen_rtcm = true;
          // Only corrections count as a working connection.
          backoff_ms_ = opt_.backoff_min_ms;
          sink_.Inject(f.bytes);
          std::scoped_lock lk(mu_);
          ++status_.frames;
        });
  };
  // True when check_head found the caster's error: the session ends here.
  auto ended = [&] {
    if (auth_failed) {
      AuthFailed();
      return true;
    }
    if (!stream_error.empty()) {
      Fail(stream_error);
      return true;
    }
    return false;
  };
  auto consume = [&](std::string_view data) {
    {
      std::scoped_lock lk(mu_);
      status_.bytes += data.size();
      last_data_ns_ = MonotonicNs();
    }
    if (!chunked) {
      forward(data);
      return true;
    }
    return dechunk.Feed(data, forward);
  };
  if (!early.empty() && !consume(early)) {
    Fail("bad chunked encoding");
    return;
  }
  if (ended()) {
    return;
  }

  std::array<char, kRecvBufferSize> buf{};
  int64_t next_gga = MonotonicNs() + MsToNs(opt_.gga_interval_ms);
  while (!StopRequested()) {
    const int64_t now = MonotonicNs();
    if (opt_.send_gga && now >= next_gga) {
      next_gga = now + MsToNs(opt_.gga_interval_ms);
      std::optional<Fix> pos;
      {
        std::scoped_lock lk(mu_);
        pos = position_;
      }
      if (pos && !internal::SendAll(fd, BuildGga(*pos))) {
        Fail("connection lost while sending GGA");
        return;
      }
    }
    int64_t last = 0;
    {
      std::scoped_lock lk(mu_);
      last = last_data_ns_;
    }
    if (now - last > MsToNs(opt_.stall_after_ms)) {
      const std::string why =
          "no corrections for " + std::to_string(opt_.stall_after_ms) + " ms";
      SetState(NtripState::kStalled, why);
      return;
    }
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, internal::kPollSliceMs) <= 0) {
      continue;
    }
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n <= 0) {
      Fail("caster closed the connection");
      return;
    }
    if (!consume(std::string_view(buf.data(), static_cast<size_t>(n)))) {
      Fail("bad chunked encoding");
      return;
    }
    if (ended()) {
      return;
    }
  }
}

std::optional<std::string> NtripClient::FetchSourcetable(
    const std::string& host, int port, int version, std::string& err) {
  const internal::UniqueFd sock =
      internal::Connect(host, port, kSourcetableConnectTimeoutMs, err);
  if (!sock) {
    return std::nullopt;
  }
  const HttpRequest request{host,
                            port,
                            version,
                            "/",
                            "",
                            "",
                            "",
                            /*close_connection=*/true};
  if (!internal::SendAll(sock.Get(), BuildRequest(request))) {
    err = "cannot send request";
    return std::nullopt;
  }
  const int64_t deadline = MonotonicNs() + MsToNs(kSourcetableReadTimeoutMs);
  std::string header_err;
  auto response =
      ReadResponseHeader(sock.Get(), deadline, host, nullptr, header_err);
  if (!response || response->icy) {
    err = "no sourcetable from " + host;
    return std::nullopt;
  }
  // The table ends with ENDSOURCETABLE; casters close the connection after
  // it, so a closed connection ends the read too.
  std::string& raw = response->body;
  std::array<char, kRecvBufferSize> buf{};
  while (MonotonicNs() < deadline &&
         raw.find("ENDSOURCETABLE") == std::string::npos) {
    pollfd p{sock.Get(), POLLIN, 0};
    if (::poll(&p, 1, internal::kPollSliceMs) <= 0) {
      continue;
    }
    const ssize_t n = ::recv(sock.Get(), buf.data(), buf.size(), 0);
    if (n <= 0) {
      break;
    }
    raw.append(buf.data(), static_cast<size_t>(n));
  }
  if (!response->chunked) {
    return std::move(raw);
  }
  std::string plain;
  internal::Dechunker d;
  (void)d.Feed(raw, [&](std::string_view part) { plain += part; });
  return plain;
}

}  // namespace lgrtk
