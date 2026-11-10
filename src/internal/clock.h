// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file clock.h
/// Monotonic time in nanoseconds and unit constants (internal, not installed).
#ifndef LGRTK_SRC_INTERNAL_CLOCK_H_
#define LGRTK_SRC_INTERNAL_CLOCK_H_

#include <time.h>

#include <cstdint>

namespace lgrtk::internal {

inline constexpr int64_t kNsPerUs = 1'000;
inline constexpr int64_t kNsPerMs = 1'000'000;
inline constexpr int64_t kNsPerS = 1'000'000'000;

/// CLOCK_MONOTONIC in ns, the time base of every `t_ns` in lgrtk.
[[nodiscard]] inline int64_t MonotonicNs() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * kNsPerS + ts.tv_nsec;
}

/// Milliseconds to nanoseconds.
[[nodiscard]] constexpr int64_t MsToNs(int64_t ms) noexcept {
  return ms * kNsPerMs;
}

}  // namespace lgrtk::internal

#endif  // LGRTK_SRC_INTERNAL_CLOCK_H_
