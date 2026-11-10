// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_CORRECTION_SINK_H_
#define LGRTK_CORRECTION_SINK_H_

/// @file correction_sink.h
/// Where differential corrections go (the receiver).

#include <cstdint>
#include <span>

namespace lgrtk {

/// Accepts RTCM3 corrections. Implemented by Lg290pReceiver; NtripClient
/// writes into it.
class CorrectionSink {
 public:
  virtual ~CorrectionSink() = default;
  /// Queues whole RTCM3 frames (one or more, never a partial frame).
  /// Thread-safe for one producer. Returns false when the queue is full.
  virtual bool Inject(std::span<const uint8_t> rtcm_frames) = 0;

 protected:
  /// Interface only: constructed, copied and moved as part of an
  /// implementation (protected, so a sink cannot be sliced).
  CorrectionSink() = default;
  /// @copydoc CorrectionSink()
  CorrectionSink(const CorrectionSink&) = default;
  /// @copydoc CorrectionSink()
  CorrectionSink& operator=(const CorrectionSink&) = default;
  /// @copydoc CorrectionSink()
  CorrectionSink(CorrectionSink&&) = default;
  /// @copydoc CorrectionSink()
  CorrectionSink& operator=(CorrectionSink&&) = default;
};

}  // namespace lgrtk

#endif  // LGRTK_CORRECTION_SINK_H_
