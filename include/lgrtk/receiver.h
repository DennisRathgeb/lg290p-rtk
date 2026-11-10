// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_RECEIVER_H_
#define LGRTK_RECEIVER_H_

/// @file receiver.h
/// The LG290P driver: an explicit state machine on its own I/O thread.
///
/// @verbatim
///  kDisconnected -> kOpening -> kIdentifying -> kConfiguring -> kStreaming <->
///  kStale
///        ^             | fail        | no reply at any baud        | | | v v
///        v            v
///        +-------- kReconnecting <---+-- port vanished / I/O
///        error-+------------+
///                                     kFault (a command was refused; waits for
///                                     Stop)
/// @endverbatim
///
/// Every transition is reported through OnStatus. Fixes arrive through
/// OnFix; LatestFix() gives the newest one to pollers, and Status() says how
/// old it is, so a consumer never mistakes the last fix before an unplug for
/// a live one.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lgrtk/correction_sink.h"
#include "lgrtk/epoch.h"
#include "lgrtk/fix.h"
#include "lgrtk/framer.h"
#include "lgrtk/lg290p.h"
#include "lgrtk/nmea.h"
#include "lgrtk/serial.h"

namespace lgrtk {

/// Driver states (see the diagram in receiver.h).
enum class State : uint8_t {
  kDisconnected,  ///< not started yet / port never opened
  kOpening,       ///< opening the tty
  kIdentifying,   ///< asking for the firmware version, detecting the baud
  kConfiguring,   ///< reading back and applying the configuration
  kStreaming,     ///< epochs arrive
  kStale,         ///< port open, but no epoch for stale_after_ms
  kReconnecting,  ///< port lost or not present; retrying with backoff
  kFault,         ///< the receiver refused a command; needs attention
  kStopped,       ///< Stop() was called
};

/// Lower-case name of a state ("streaming", ...).
[[nodiscard]] const char* ToString(State s) noexcept;

/// One state transition.
struct StatusEvent {
  State from;          ///< previous state
  State to;            ///< new state
  std::string reason;  ///< human-readable cause
  int64_t t_ns;        ///< CLOCK_MONOTONIC
};

/// Options for Lg290pReceiver.
struct ReceiverOptions {
  std::string port = "/dev/ttyACM0";  ///< tty path (a udev symlink is best)
  int baud = 460800;                  ///< tried first (LG290P factory default)
  /// Tried after `baud` when the receiver does not answer.
  std::vector<int> baud_candidates{460800, 921600, 115200, 230400, 9600};
  lg290p::ReceiverConfig config =
      lg290p::DefaultRoverConfig();  ///< desired settings
  bool apply_config = true;          ///< read back and apply `config`
  int stale_after_ms = 500;          ///< no epoch for this long -> kStale
  int backoff_min_ms = 250;          ///< first reconnect delay
  int backoff_max_ms = 5000;         ///< reconnect delay cap
  int command_timeout_ms = 1000;     ///< wait for a reply
  int command_retries = 2;           ///< extra attempts before kFault
};

/// Snapshot for status displays.
struct ReceiverStatus {
  State state = State::kDisconnected;  ///< current state
  std::string firmware;                ///< PQTMVERNO version string
  std::string port;                    ///< tty path
  int baud = 0;                        ///< current line speed
  uint64_t bytes = 0;                  ///< bytes read
  uint64_t epochs = 0;                 ///< fixes emitted
  uint64_t reconnects = 0;             ///< successful re-opens after a loss
  uint64_t rtcm_injected = 0;     ///< correction bytes written to the receiver
  uint64_t inject_dropped = 0;    ///< correction bytes refused (queue full)
  StreamFramer::Counters framer;  ///< frame statistics
  int64_t last_fix_age_ns = -1;   ///< -1 before the first fix
  std::string last_error;         ///< most recent problem
};

/// Writes the raw receiver byte stream to a file (for post-processing:
/// `convbin -r rtcm3`, RTKLIB). Use it from OnRaw.
class RawRecorder {
 public:
  /// Opens (truncates) `path`.
  static std::optional<RawRecorder> Open(const std::string& path,
                                         std::string& err);
  /// Moves the open file.
  RawRecorder(RawRecorder&& o) noexcept;
  /// Closes this file and takes over `o`'s.
  RawRecorder& operator=(RawRecorder&& o) noexcept;
  RawRecorder(const RawRecorder&) = delete;
  RawRecorder& operator=(const RawRecorder&) = delete;
  ~RawRecorder();
  /// Appends bytes (buffered).
  void Write(std::span<const uint8_t> bytes) noexcept;
  /// Bytes written so far.
  [[nodiscard]] uint64_t Bytes() const noexcept { return bytes_; }

 private:
  explicit RawRecorder(std::FILE* f) : f_(f) {}
  std::FILE* f_ = nullptr;
  uint64_t bytes_ = 0;
};

/// The LG290P driver. Set callbacks before Start(); they run on the I/O
/// thread and must return quickly; whatever they capture must outlive Stop()
/// (or the receiver). All other methods are thread-safe.
class Lg290pReceiver final : public CorrectionSink {
 public:
  /// Creates the driver; nothing is opened until Start().
  explicit Lg290pReceiver(ReceiverOptions options);
  ~Lg290pReceiver() override;
  Lg290pReceiver(const Lg290pReceiver&) = delete;
  Lg290pReceiver& operator=(const Lg290pReceiver&) = delete;

  void OnFix(std::function<void(const Fix&)> cb) {
    on_fix_ = std::move(cb);
  }  ///< every epoch
  void OnStatus(std::function<void(const StatusEvent&)> cb) {
    on_status_ = std::move(cb);
  }  ///< transitions
  void OnNmea(std::function<void(std::string_view)> cb) {
    on_nmea_ = std::move(cb);
  }  ///< every sentence
  void OnRtcm(std::function<void(std::span<const uint8_t>)> cb) {
    on_rtcm_ = std::move(cb);
  }  ///< RTCM out (raw obs, base)
  void OnRaw(std::function<void(std::span<const uint8_t>)> cb) {
    on_raw_ = std::move(cb);
  }  ///< every byte read

  /// Starts the I/O thread. False if already running.
  bool Start();
  /// Stops the thread and closes the port; joins within ~1 s. Idempotent.
  void Stop();

  /// State, counters, firmware and the age of the latest fix.
  [[nodiscard]] ReceiverStatus Status() const;
  /// The newest fix, if any (check Status().last_fix_age_ns for freshness).
  [[nodiscard]] std::optional<Fix> LatestFix() const;
  /// Queues a command; the future gets the reply (nullopt on timeout or when
  /// the command expects none). Executed while streaming.
  std::future<std::optional<nmea::OwnedReply>> Send(lg290p::Command cmd);
  /// Queues RTCM3 frames for the receiver (CorrectionSink).
  bool Inject(std::span<const uint8_t> rtcm_frames) override;

 private:
  struct Pending {
    lg290p::Command cmd;
    std::promise<std::optional<nmea::OwnedReply>> promise;
  };

  void Run();
  void SetState(State s, std::string reason);
  bool TryOpen();
  bool Identify();
  bool Configure();
  void StreamOnce();
  void BackoffWait();
  /// Reads once (up to `timeout_ms`) and processes everything that arrived.
  /// Returns false when the port failed.
  bool Pump(int timeout_ms);
  void OnFrame(const Frame& f, int64_t t_arrival);
  /// Sends `cmd` and waits for its reply. Returns false on port failure;
  /// `reply` stays empty on timeout.
  bool Execute(const lg290p::Command& cmd,
               std::optional<nmea::OwnedReply>& reply);
  void DrainInject();
  void ServeQueue();
  void FailPendingQueue();
  bool StopRequested() const { return stop_.load(std::memory_order_relaxed); }

  ReceiverOptions opt_;
  std::function<void(const Fix&)> on_fix_;
  std::function<void(const StatusEvent&)> on_status_;
  std::function<void(std::string_view)> on_nmea_;
  std::function<void(std::span<const uint8_t>)> on_rtcm_;
  std::function<void(std::span<const uint8_t>)> on_raw_;

  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};

  // I/O thread only.
  std::optional<SerialPort> port_;
  StreamFramer framer_;
  EpochAssembler epoch_;
  const lg290p::Command* awaiting_ = nullptr;
  std::optional<nmea::OwnedReply>* awaiting_reply_ = nullptr;
  int backoff_ms_ = 0;
  bool had_connection_ = false;
  int64_t last_fix_t_ = 0;
  double last_utc_ = -1.0;
  std::optional<Pending> in_flight_;
  int64_t in_flight_deadline_ = 0;
  bool reidentify_ = false;

  // Shared state.
  mutable std::mutex mu_;
  ReceiverStatus status_;
  std::optional<Fix> latest_;
  std::deque<Pending> queue_;

  // Single-producer single-consumer byte ring for corrections.
  static constexpr size_t kRingSize = 64 * 1024;
  std::unique_ptr<uint8_t[]> ring_;
  std::atomic<size_t> ring_head_{0};  // written by the producer
  std::atomic<size_t> ring_tail_{0};  // written by the I/O thread
};

}  // namespace lgrtk

#endif  // LGRTK_RECEIVER_H_
