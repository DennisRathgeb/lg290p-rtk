// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_FIX_H_
#define LGRTK_FIX_H_

/// @file fix.h
/// The normalised position record: one per receiver epoch.

#include <cstdint>

namespace lgrtk {

/// Solution type, from the GGA quality indicator.
enum class FixType : uint8_t {
  kNone,           ///< no position
  kSingle,         ///< autonomous (standard positioning)
  kDgnss,          ///< code differential / SBAS
  kPps,            ///< PPS mode
  kRtkFixed,       ///< RTK, integer ambiguities fixed (cm)
  kRtkFloat,       ///< RTK, float ambiguities (dm)
  kDeadReckoning,  ///< estimated
  kManual,         ///< manual input / base fixed position
  kSimulation,     ///< simulator
};

/// Short lower-case name ("rtk-fixed", ...).
[[nodiscard]] const char* ToString(FixType t) noexcept;
/// Maps a GGA quality indicator (0..8) to a FixType; unknown values → kNone.
[[nodiscard]] FixType FixTypeFromGgaQuality(uint8_t quality) noexcept;

/// Which reference frame the position is in. The receiver cannot know the
/// frame of its corrections; the application declares the CRS of each case
/// (e.g. CHTRS95 for swipos corrections, WGS 84 ≈ ITRF for autonomous).
enum class FixFrame : uint8_t {
  kAutonomous,  ///< no differential corrections used: broadcast-ephemeris frame
  kCorrected,  ///< differential corrections used: the correction source's frame
};

/// Where the 1-sigma values came from.
enum class SigmaSource : uint8_t {
  kNone,  ///< no error estimate this epoch (sigmas are 0)
  kGst,   ///< GST sentence (per-axis 1-sigma)
  kEpe,   ///< PQTMEPE (estimated position error)
};

/// One receiver epoch. Trivially copyable.
struct Fix {
  int64_t t_arrival_ns;  ///< CLOCK_MONOTONIC when the epoch's last sentence was
                         ///< read
  double utc_s;          ///< UTC seconds of day of the epoch
  int32_t date_ymd;      ///< UTC date, e.g. 20261110; 0 while unknown
  uint32_t seq;          ///< increments by one per emitted fix

  double lat_deg;  ///< deg, north positive (valid if has_position)
  double lon_deg;  ///< deg, east positive
  double h_ell_m;  ///< m, ellipsoidal height (GGA altitude + geoid separation)
  double geoid_sep_m;  ///< m, the receiver's built-in geoid separation

  float sigma_e_m;   ///< m, east 1-sigma
  float sigma_n_m;   ///< m, north 1-sigma
  float sigma_u_m;   ///< m, up 1-sigma
  float vel_e_mps;   ///< m/s, east velocity (valid if vel_valid)
  float vel_n_mps;   ///< m/s, north velocity
  float vel_u_mps;   ///< m/s, up velocity (0: not reported in NMEA)
  float hdop;        ///< dimensionless
  float pdop;        ///< dimensionless, latest GSA (0 if none)
  float diff_age_s;  ///< s, age of the differential corrections (0 if none)

  uint16_t station_id;       ///< reference station id of the corrections
  uint8_t sats;              ///< satellites used
  FixType type;              ///< solution type
  FixFrame frame;            ///< autonomous or corrected
  SigmaSource sigma_source;  ///< origin of the sigmas
  bool has_position;         ///< lat/lon/h are meaningful
  bool vel_valid;            ///< horizontal velocity is meaningful
};

}  // namespace lgrtk

#endif  // LGRTK_FIX_H_
