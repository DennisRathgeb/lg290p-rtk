// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// The gpsd JSON protocol (https://gpsd.io/gpsd_json.html), the subset lgrtkd
/// speaks: VERSION, DEVICES, WATCH, POLL, TPV, GST, SKY, ERROR. Every encoder
/// returns one object terminated by "\r\n", as gpsd sends them.
#ifndef LGRTK_SRC_DAEMON_GPSD_JSON_H_
#define LGRTK_SRC_DAEMON_GPSD_JSON_H_

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "lgrtk/fix.h"

namespace lgrtk::daemon {

/// UTC of the fix in ns since 1970; nullopt while the receiver has no date.
[[nodiscard]] std::optional<int64_t> FixUnixNs(const Fix& fix);
/// ISO 8601 with milliseconds, "2026-11-10T12:00:00.100Z".
[[nodiscard]] std::string IsoTime(int64_t unix_ns);

/// VERSION greeting: release "lgrtkd <version>", protocol 3.14.
[[nodiscard]] std::string VersionJson();
/// DEVICES with our one device; `active` false reports it deactivated.
[[nodiscard]] std::string DevicesJson(const std::string& device, bool active,
                                      int baud);
/// TPV. mode 3 with a position, else 1; status 3 RTK fixed, 4 RTK float,
/// 2 DGNSS, 1 otherwise; epx/epy/epv are 1.96 sigma (about 95 %).
[[nodiscard]] std::string TpvJson(const Fix& fix, const std::string& device);
/// GST with the 1-sigma errors; "" when the fix carries no sigmas.
[[nodiscard]] std::string GstJson(const Fix& fix, const std::string& device);
/// TPV with mode 0, sent when the receiver is lost.
[[nodiscard]] std::string NoFixTpvJson(const std::string& device);

/// What a client asked to receive.
struct Watch {
  bool enable = false;  ///< streaming on
  bool json = false;    ///< TPV/GST/SKY objects
  bool nmea = false;    ///< raw NMEA sentences
};
/// WATCH reply describing `w`.
[[nodiscard]] std::string WatchJson(const Watch& w);
/// Updates `w` from a WATCH argument; keys with the wrong type are ignored.
void ApplyWatch(const nlohmann::json& arg, Watch* w);
/// ERROR object.
[[nodiscard]] std::string ErrorJson(std::string_view message);

/// A client command line, e.g. `?WATCH={"enable":true};`.
struct GpsdCommand {
  /// Command name.
  enum Kind { kVersion, kWatch, kPoll, kDevices, kUnknown } kind = kUnknown;
  nlohmann::json arg;  ///< the object after '=', null if absent
};
/// Parses one line (without the newline). nullopt when the argument is not
/// a JSON object; an unknown command gives kUnknown.
[[nodiscard]] std::optional<GpsdCommand> ParseGpsdCommand(
    std::string_view line);

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_GPSD_JSON_H_
