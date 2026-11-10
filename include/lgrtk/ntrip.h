// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_NTRIP_H_
#define LGRTK_NTRIP_H_

/// @file ntrip.h
/// NTRIP client: downloads RTCM3 corrections from a caster and feeds them to
/// a CorrectionSink (the receiver).

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "lgrtk/correction_sink.h"
#include "lgrtk/fix.h"

namespace lgrtk {

/// Client states.
enum class NtripState : uint8_t {
  kIdle,        ///< not started
  kConnecting,  ///< TCP connect + request
  kStreaming,   ///< corrections arrive
  kStalled,     ///< connected but no data for stall_after_ms (reconnects)
  kBackoff,     ///< waiting before the next attempt
  kAuthFailed,  ///< HTTP 401: no further attempts until Stop(), then Start()
  kNoPosition,  ///< send_gga is on and no position is known yet
  kStopped,     ///< Stop() was called
};

/// Lower-case name ("streaming", ...).
[[nodiscard]] const char* ToString(NtripState s) noexcept;

/// Caster connection settings.
struct NtripOptions {
  std::string host;        ///< caster host name or address
  int port = 2101;         ///< caster port
  std::string mountpoint;  ///< mountpoint without leading '/'
  std::string user;        ///< user name (may be empty)
  std::string password;    ///< password (never logged)
  int version = 2;         ///< NTRIP 1 or 2
  bool send_gga = true;  ///< VRS / nearest-base casters need the rover position
  int gga_interval_ms = 10000;    ///< GGA upload period
  int stall_after_ms = 10000;     ///< no data for this long -> reconnect
  int connect_timeout_ms = 5000;  ///< TCP connect + response header
  int backoff_min_ms = 1000;      ///< first retry delay
  int backoff_max_ms = 60000;     ///< retry delay cap
};

/// Snapshot of the client.
struct NtripStatus {
  NtripState state = NtripState::kIdle;  ///< current state
  uint64_t bytes = 0;                    ///< correction bytes received
  uint64_t frames = 0;                   ///< RTCM frames forwarded
  uint64_t connects = 0;                 ///< successful connections
  int64_t last_data_age_ns = -1;         ///< -1 before the first data
  int http_status = 0;                   ///< last HTTP status (0 for ICY)
  std::string last_error;  ///< most recent problem (no credentials)
};

/// "$GPGGA,...*HH\r\n" for a caster from a fix (position as the receiver
/// reported it: altitude = h_ell - geoid separation).
[[nodiscard]] std::string BuildGga(const Fix& f);
/// RFC 4648 base64 (for HTTP Basic auth).
[[nodiscard]] std::string Base64(std::string_view in);

/// NTRIP v1/v2 client on its own thread. RTCM frames are reassembled from
/// the byte stream (v2 chunked transfer decoded) and injected whole.
class NtripClient {
 public:
  /// `sink` must outlive the client.
  NtripClient(NtripOptions options, CorrectionSink& sink);
  ~NtripClient();
  NtripClient(const NtripClient&) = delete;
  NtripClient& operator=(const NtripClient&) = delete;

  /// Latest rover position for GGA upload. Thread-safe; call from OnFix.
  void SetPosition(const Fix& fix);
  /// State changes (old, new, reason). Set before Start(); runs on the client
  /// thread.
  void OnStatus(
      std::function<void(NtripState, NtripState, const std::string&)> cb) {
    on_status_ = std::move(cb);
  }
  /// Starts the thread; false if already running.
  bool Start();
  /// Stops and joins within ~1 s (a DNS lookup in progress can take longer;
  /// use an IP address where that matters). Idempotent.
  void Stop();
  /// State, counters and the age of the last correction data.
  [[nodiscard]] NtripStatus Status() const;

  /// Downloads a caster's sourcetable (list of mountpoints).
  [[nodiscard]] static std::optional<std::string> FetchSourcetable(
      const std::string& host, int port, int version, std::string& err);

 private:
  void Run();
  void SetState(NtripState s, const std::string& reason);
  /// One connection; returns when it ends. Sets state/err on failure.
  void Session();
  /// Forwards corrections from the connected socket `fd` until the
  /// connection ends, stalls or Stop() is called. `early` holds bytes that
  /// arrived with the response header.
  void StreamLoop(int fd, bool chunked, std::string_view early);
  /// Records `why` as the last error.
  void Fail(const std::string& why);
  bool BackoffWait();
  bool StopRequested() const { return stop_.load(std::memory_order_relaxed); }

  NtripOptions opt_;
  CorrectionSink& sink_;
  std::function<void(NtripState, NtripState, const std::string&)> on_status_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  int backoff_ms_ = 0;

  mutable std::mutex mu_;
  NtripStatus status_;
  std::optional<Fix> position_;
  int64_t last_data_ns_ = 0;
};

}  // namespace lgrtk

#endif  // LGRTK_NTRIP_H_
