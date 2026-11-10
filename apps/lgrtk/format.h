// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// lgrtk CLI: how fixes, state changes and reports are printed.
#ifndef LGRTK_APPS_LGRTK_FORMAT_H_
#define LGRTK_APPS_LGRTK_FORMAT_H_

#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "lgrtk/fix.h"
#include "lgrtk/nmea.h"
#include "lgrtk/receiver.h"

namespace lgrtk::cli {

/// One line per fix: time, type, position, sigmas, sats, HDOP, age.
[[nodiscard]] std::string FormatFix(const Fix& f);

/// "[<what> from -> to] reason" on `out` (no "<what> " when `what` is empty),
/// flushed.
void PrintTransition(std::FILE* out, std::string_view what, const char* from,
                     const char* to, std::string_view reason);
/// A receiver state change, as PrintTransition.
void PrintEvent(std::FILE* out, std::string_view what, const StatusEvent& e);

/// What `lgrtk probe` counts while it listens.
struct ProbeStats {
  std::map<std::string, int> types;   ///< sentences per NMEA type
  std::map<int, int> quality;         ///< GGA fixes per quality value
  std::optional<nmea::Gga> last_gga;  ///< latest GGA
  double first_utc_s = -1;            ///< UTC of the first GGA, -1 none
  double last_utc_s = -1;             ///< UTC of the latest GGA, -1 none
  int backwards = 0;                  ///< GGA whose UTC went backwards

  /// Counts one NMEA sentence.
  void Add(std::string_view line);
};

/// The probe report on stdout; `seconds` is how long the probe listened.
void PrintProbeReport(const ReceiverStatus& st, const ProbeStats& stats,
                      double seconds);
/// The one-line `lgrtk status` summary (plus the last error) on stdout.
void PrintStatusReport(const ReceiverStatus& st);

}  // namespace lgrtk::cli

#endif  // LGRTK_APPS_LGRTK_FORMAT_H_
