// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// chrony SOCK refclock: one sample per fix with the receiver's UTC.
/// chrony.conf: `refclock SOCK /run/chrony.lgrtk.sock refid GNSS ...`.
#ifndef LGRTK_SRC_DAEMON_CHRONY_H_
#define LGRTK_SRC_DAEMON_CHRONY_H_

#include <sys/time.h>

#include <cstdint>
#include <optional>
#include <string>

#include "internal/unique_fd.h"
#include "internal/unix_socket.h"
#include "lgrtk/fix.h"

namespace lgrtk::daemon {

/// chrony's `struct sock_sample` (refclock_sock.c), 40 bytes on LP64.
struct ChronySample {
  timeval tv;     ///< local system time when the sample was taken
  double offset;  ///< receiver time minus `tv`, seconds
  int pulse;      ///< 0: not a PPS pulse
  int leap;       ///< 0: no leap second pending
  int pad;        ///< padding
  int magic;      ///< 0x534f434b ("SOCK")
};

/// A sample for `fix` read at `arrival_realtime_ns` (CLOCK_REALTIME).
/// nullopt without a date or a position. Any offset is reported: a system
/// without RTC may boot far off, and chrony's `makestep` decides.
[[nodiscard]] std::optional<ChronySample> MakeChronySample(
    const Fix& fix, int64_t arrival_realtime_ns);

/// Sends samples to chrony's socket. Silent when chrony is not running; each
/// send retries, so starting chrony later works.
class ChronySock {
 public:
  /// `path` is the socket chrony listens on.
  explicit ChronySock(const std::string& path);
  /// Sends one sample (best effort).
  void Send(const ChronySample& s);
  /// Samples delivered.
  [[nodiscard]] uint64_t Sent() const { return sent_; }

 private:
  internal::UniqueFd fd_;
  internal::UnixAddress addr_;
  bool addr_ok_ = false;
  uint64_t sent_ = 0;
};

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_CHRONY_H_
