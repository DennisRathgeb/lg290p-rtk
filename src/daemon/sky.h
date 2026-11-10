// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// Satellites in view (GSV) and in use (GSA), for gpsd SKY reports.
#ifndef LGRTK_SRC_DAEMON_SKY_H_
#define LGRTK_SRC_DAEMON_SKY_H_

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lgrtk::daemon {

/// One satellite as gpsd reports it.
struct SkySat {
  uint16_t prn;           ///< satellite number as the receiver reports it
  int16_t elevation_deg;  ///< elevation, -1 unknown
  int16_t azimuth_deg;    ///< azimuth, -1 unknown
  int16_t cn0_dbhz;       ///< strongest C/N0, -1 not tracked
  bool used;              ///< in the latest solution (GSA)
  int gnssid;  ///< gpsd/u-blox id: 0 GPS, 1 SBAS, 2 Galileo, 3 BeiDou,
               ///< 5 QZSS, 6 GLONASS, 7 NavIC
};

/// Collects GSV groups (per constellation and signal) and GSA used lists.
/// A GSV group replaces the previous one only once its last sentence arrived.
class SkyTracker {
 public:
  /// Feeds one NMEA sentence; everything except valid GSV/GSA is ignored.
  void FeedNmea(std::string_view line);
  /// Satellites in view, sorted by gnssid and PRN; signals merged.
  [[nodiscard]] std::vector<SkySat> Satellites() const;
  /// Latest GSA HDOP (0 before any).
  [[nodiscard]] float Hdop() const { return hdop_; }
  /// Latest GSA PDOP (0 before any).
  [[nodiscard]] float Pdop() const { return pdop_; }

 private:
  struct InView {
    uint16_t prn;
    int16_t elevation_deg;
    int16_t azimuth_deg;
    int16_t cn0_dbhz;
  };
  using GroupKey = std::pair<int, int>;  // gnssid of the talker, signal id
  void FeedGsv(std::string_view line);
  void FeedGsa(std::string_view line);

  std::map<GroupKey, std::vector<InView>> groups_;   // complete groups
  std::map<GroupKey, std::vector<InView>> partial_;  // being received
  std::map<int, std::set<uint16_t>> used_;  // per gnssid of the GSA system
  float hdop_ = 0;
  float pdop_ = 0;
};

/// gpsd SKY object (one line, "\r\n"); `unix_ns` adds "time" when known.
[[nodiscard]] std::string SkyJson(const SkyTracker& sky,
                                  const std::string& device,
                                  std::optional<int64_t> unix_ns);

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_SKY_H_
