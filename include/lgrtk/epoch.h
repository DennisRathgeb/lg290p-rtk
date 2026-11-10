// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_EPOCH_H_
#define LGRTK_EPOCH_H_

/// @file epoch.h
/// Groups the NMEA sentences of one receiver epoch into one Fix.

#include <cstdint>
#include <string_view>

#include "lgrtk/fix.h"

namespace lgrtk {

/// Assembles Fix records from a sentence stream.
///
/// An epoch is the set of sentences with the same UTC time (EPE carries no
/// time and belongs to the current epoch). It closes as soon as every
/// sentence type seen in the previous epoch has arrived again (so latency is
/// one sentence, not one epoch), or at the latest when the next epoch's
/// first sentence arrives. GGA is required for a fix. Does not allocate.
class EpochAssembler {
 public:
  /// Feeds one NMEA sentence ("$...*HH", checksum checked). Returns true and
  /// fills `out` when an epoch closed.
  bool Feed(std::string_view nmea_line, int64_t t_arrival_ns, Fix& out) noexcept;
  /// Closes the pending epoch (on stale data or stop); true if one was open.
  bool Flush(Fix& out) noexcept;
  /// Forgets everything (after a reconnect); keeps the sequence counter.
  void Reset() noexcept;

 private:
  enum Bit : uint8_t { kGga = 1, kRmc = 2, kGst = 4, kEpe = 8, kZda = 16 };
  bool Emit(Fix& out) noexcept;
  void StartEpoch(double utc_s) noexcept;

  Fix pending_{};
  double pending_utc_ = -1.0;
  uint8_t seen_ = 0;      // sentence types of the pending epoch
  uint8_t expected_ = 0;  // sentence types of the previous complete epoch
  bool emitted_ = false;  // the pending epoch was already emitted
  bool open_ = false;     // an epoch is pending
  int64_t last_t_ = 0;
  float last_pdop_ = 0.0f;
  int32_t last_date_ = 0;
  uint32_t seq_ = 0;
};

}  // namespace lgrtk

#endif  // LGRTK_EPOCH_H_
