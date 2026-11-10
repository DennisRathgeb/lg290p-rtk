// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_NMEA_H_
#define LGRTK_NMEA_H_

/// @file nmea.h
/// Parsers for the NMEA 0183 sentences and Quectel `$PQTM` messages the
/// LG290P sends. Every parser accepts a full sentence ("$...*HH", checksum
/// verified, CR/LF optional) or a bare body; none allocates. Field layouts
/// follow the Quectel LG290P(03) GNSS Protocol Specification v1.1.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lgrtk::nmea {

/// Maximum number of comma-separated fields considered in one sentence.
inline constexpr size_t kMaxFields = 32;

/// The constellation a talker id stands for.
enum class Talker : uint8_t {
  kUnknown,
  kMulti,
  kGps,
  kGlonass,
  kGalileo,
  kBeidou,
  kQzss,
  kNavic
};

/// Body between '$' and '*' when the checksum is correct, else empty.
[[nodiscard]] std::string_view Validate(std::string_view line) noexcept;
/// Splits `body` at commas into `out`; returns the number of fields.
size_t SplitFields(std::string_view body,
                   std::span<std::string_view> out) noexcept;
/// "GGA" for "GNGGA,...", "PQTMEPE" for "PQTMEPE,...", empty if unknown.
[[nodiscard]] std::string_view SentenceType(std::string_view body) noexcept;
/// Talker of a standard sentence body ("GN" → kMulti, "GP" → kGps, ...).
[[nodiscard]] Talker TalkerOf(std::string_view body) noexcept;

/// GGA: time, position, fix quality.
struct Gga {
  double utc_s;         ///< s, UTC seconds of day
  double lat_deg;       ///< deg, north positive
  double lon_deg;       ///< deg, east positive
  double alt_msl_m;     ///< m, altitude above the receiver's geoid model
  double geoid_sep_m;   ///< m, geoid separation (ellipsoid height = alt + sep)
  double hdop;          ///< dimensionless
  double diff_age_s;    ///< s, age of differential corrections
  uint16_t station_id;  ///< reference station id (0 if empty)
  uint8_t
      quality;   ///< 0 none, 1 single, 2 DGNSS, 4 RTK fixed, 5 RTK float, ...
  uint8_t sats;  ///< satellites used
  bool has_position;  ///< lat/lon fields present
  bool has_diff_age;  ///< differential age field present
};

/// RMC: validity, speed, course, date.
struct Rmc {
  double utc_s;       ///< s, UTC seconds of day
  double lat_deg;     ///< deg
  double lon_deg;     ///< deg
  double speed_mps;   ///< m/s over ground
  double course_deg;  ///< deg true, 0 if empty
  int32_t
      date_ymd;  ///< e.g. 20261110; 0 while the receiver does not know the date
  bool valid;    ///< status 'A'
  bool has_course;  ///< course field present
};

/// GST: error statistics (1-sigma, metres).
struct Gst {
  double utc_s;        ///< s, UTC seconds of day
  double rms_m;        ///< RMS of the pseudorange residuals
  double major_m;      ///< semi-major axis of the error ellipse
  double minor_m;      ///< semi-minor axis
  double orient_deg;   ///< orientation of the semi-major axis from north
  double sigma_lat_m;  ///< latitude (north) 1-sigma
  double sigma_lon_m;  ///< longitude (east) 1-sigma
  double sigma_alt_m;  ///< altitude 1-sigma
  bool valid;          ///< fields present (empty before the first fix)
};

/// GSA: DOPs and satellites used, one sentence per constellation.
struct Gsa {
  double pdop, hdop, vdop;  ///< dimensionless
  uint8_t fix_mode;         ///< 1 none, 2 2-D, 3 3-D
  uint8_t system_id;   ///< 1 GPS, 2 GLONASS, 3 Galileo, 4 BDS, 5 QZSS, 6 NavIC
  uint8_t used_count;  ///< satellites listed as used
};

/// One satellite of a GSV sentence.
struct SatInView {
  uint16_t prn;      ///< satellite id
  int16_t elev_deg;  ///< -1 if unknown
  int16_t az_deg;    ///< -1 if unknown
  int16_t cn0_dbhz;  ///< -1 if not tracked
};

/// GSV: satellites in view, up to four per sentence.
struct Gsv {
  Talker talker;                  ///< constellation
  uint8_t total_msgs;             ///< sentences in this group
  uint8_t msg_num;                ///< 1-based index
  uint8_t in_view;                ///< satellites in view for this signal
  uint8_t signal_id;              ///< NMEA 4.10 signal id (0 if absent)
  uint8_t count;                  ///< valid entries in `sats`
  std::array<SatInView, 4> sats;  ///< satellites
};

/// VTG: course and speed.
struct Vtg {
  double course_deg;  ///< deg true
  double speed_mps;   ///< m/s
};

/// ZDA: time and date.
struct Zda {
  double utc_s;      ///< s, UTC seconds of day
  int32_t date_ymd;  ///< e.g. 20211223, 0 if unknown
};

/// PQTMEPE (version 2): estimated position error, metres.
struct Epe {
  double north_m, east_m, down_m, h2d_m, h3d_m;  ///< metres
};

/// PQTMVERNO: firmware version. Views point into the parsed line.
struct Verno {
  std::string_view version;     ///< e.g. "LG290P03AANR01A06S"
  std::string_view build_date;  ///< "YYYY/MM/DD"
  std::string_view build_time;  ///< "hh:mm:ss"
};

/// PQTMSVINSTATUS: survey-in progress of a base station.
struct SvinStatus {
  uint32_t tow_ms;               ///< GPS time of week
  uint8_t valid;                 ///< 0 invalid/off, 1 in progress, 2 valid
  uint32_t observations;         ///< positions averaged so far
  uint32_t configured_duration;  ///< configured minimum positions
  double mean_x_m, mean_y_m, mean_z_m;  ///< ECEF mean position
  double mean_acc_m;                    ///< mean position accuracy
};

/// Reply of the receiver to a `$PQTM` command: "$PQTMxxx,OK[,values]" or
/// "$PQTMxxx,ERROR,<code>". Views point into the parsed line.
struct Reply {
  std::string_view command;  ///< e.g. "PQTMCFGFIXRATE"
  bool ok;                   ///< OK vs ERROR
  int error_code;            ///< 1 invalid parameters, 2 failed, 3 unsupported
  std::span<const std::string_view> values;  ///< fields after OK
};

/// A Reply that owns its strings, for passing across threads.
struct OwnedReply {
  std::string command;              ///< e.g. "PQTMCFGFIXRATE"
  bool ok = false;                  ///< OK vs ERROR
  int error_code = 0;               ///< see Reply
  std::vector<std::string> values;  ///< fields after OK
};

[[nodiscard]] std::optional<Gga> ParseGga(std::string_view line) noexcept;
[[nodiscard]] std::optional<Rmc> ParseRmc(std::string_view line) noexcept;
[[nodiscard]] std::optional<Gst> ParseGst(std::string_view line) noexcept;
[[nodiscard]] std::optional<Gsa> ParseGsa(std::string_view line) noexcept;
[[nodiscard]] std::optional<Gsv> ParseGsv(std::string_view line) noexcept;
[[nodiscard]] std::optional<Vtg> ParseVtg(std::string_view line) noexcept;
[[nodiscard]] std::optional<Zda> ParseZda(std::string_view line) noexcept;
[[nodiscard]] std::optional<Epe> ParseEpe(std::string_view line) noexcept;
[[nodiscard]] std::optional<Verno> ParseVerno(std::string_view line) noexcept;
[[nodiscard]] std::optional<SvinStatus> ParseSvinStatus(
    std::string_view line) noexcept;
/// Parses a command reply; `fields` is scratch space the reply's values view.
[[nodiscard]] std::optional<Reply> ParseReply(
    std::string_view line, std::span<std::string_view> fields) noexcept;
/// Copies a reply into owned strings.
[[nodiscard]] OwnedReply Own(const Reply& r);

}  // namespace lgrtk::nmea

#endif  // LGRTK_NMEA_H_
