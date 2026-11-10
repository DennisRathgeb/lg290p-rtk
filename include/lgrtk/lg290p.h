// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_LG290P_H_
#define LGRTK_LG290P_H_

/// @file lg290p.h
/// The Quectel LG290P `$PQTM` command set as typed builders, plus the
/// read-back-and-diff configuration planner. Strings and ranges follow the
/// LG290P(03) GNSS Protocol Specification v1.1. Builders that take a value
/// with a documented range return std::nullopt for values outside it.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lgrtk/nmea.h"

namespace lgrtk::lg290p {

/// One command ready to send.
struct Command {
  std::string text;  ///< "$...*HH\r\n"
  std::string
      reply_to;  ///< reply sentence name; empty: the receiver does not answer
  std::string match_value;   ///< first reply value that identifies the answer
                             ///< (MSGRATE reads)
  bool needs_reset = false;  ///< takes effect only after save + reset
  bool is_write = false;     ///< changes receiver state
};

/// "$<body>*HH\r\n".
[[nodiscard]] std::string BuildCommand(std::string_view body);
/// True when `reply` answers `cmd`.
[[nodiscard]] bool Matches(const Command& cmd,
                           const nmea::Reply& reply) noexcept;
/// Same for an owned reply.
[[nodiscard]] bool Matches(const Command& cmd,
                           const nmea::OwnedReply& reply) noexcept;

/// Receiver operating mode.
enum class Mode { kRover = 1, kBase = 2 };
/// Restart kinds.
enum class Start { kHot, kWarm, kCold };

/// Output rate of one message: emitted once every `rate` fixes (0 = off).
struct MessageRate {
  std::string name;  ///< "GGA", "GST", "PQTMEPE", "RTCM3-107X", ...
  int rate = 0;      ///< 0..255 (fixes per output)
  int version = 0;   ///< PQTM message version / RTCM offset; 0 = omit
  bool operator==(const MessageRate&) const = default;
};
/// NMEA decimal places (PQTMCFGNMEADP).
struct NmeaDecimals {
  int utc = 3, pos = 8, alt = 3, dop = 2, spd = 3, cog = 2;  ///< digits
  bool operator==(const NmeaDecimals&) const = default;
};
/// RTK engine settings (PQTMCFGRTK).
struct RtkSettings {
  int diff_mode = 1;    ///< 0 off, 1 auto (RTK), 2 RTD only
  int rel_mode = 1;     ///< 1 absolute, 2 relative
  int timeout_s = 120;  ///< max differential age for an RTK fix, 1..600
  bool operator==(const RtkSettings&) const = default;
};
/// Enabled constellations (PQTMCFGCNST).
struct Constellations {
  bool gps = true, glonass = true, galileo = true, beidou = true, qzss = true,
       navic = true;  ///< on/off
  bool operator==(const Constellations&) const = default;
};
/// PPS output (PQTMCFGPPS, index 1).
struct PpsSettings {
  bool enable = true;     ///< output on
  int duration_ms = 100;  ///< pulse width 0..900
  int mode = 1;           ///< 1 always, 2 only with a fix
  int polarity = 1;       ///< 1 positive
  bool operator==(const PpsSettings&) const = default;
};

/// Desired (or read-back) receiver settings. Unset fields are not managed.
struct ReceiverConfig {
  std::optional<int> fix_interval_ms;            ///< 50..1000 ms (needs reset)
  std::vector<MessageRate> message_rates;        ///< per message
  std::optional<NmeaDecimals> nmea_decimals;     ///< NMEA precision
  std::optional<RtkSettings> rtk;                ///< RTK engine
  std::optional<Constellations> constellations;  ///< (needs reset)
  std::optional<double> elevation_mask_deg;      ///< 0..90
  std::optional<double> cnr_mask_dbhz;           ///< 0..99
  std::optional<Mode> receiver_mode;             ///< rover/base (needs reset)
  std::optional<PpsSettings> pps;                ///< PPS
};

/// A sensible rover configuration: 10 Hz, high-precision NMEA, GGA/RMC/GST/
/// EPE every fix, GLL/VTG off, RTK auto with 30 s max differential age.
[[nodiscard]] ReceiverConfig DefaultRoverConfig();

[[nodiscard]] Command QueryVersion();
[[nodiscard]] Command QueryUniqueId();
[[nodiscard]] std::optional<Command> SetFixRate(int interval_ms);
[[nodiscard]] Command GetFixRate();
[[nodiscard]] Command SetMessageRate(std::string_view msg, int rate,
                                     int version = 0);
[[nodiscard]] Command GetMessageRate(std::string_view msg, int version = 0);
[[nodiscard]] Command SetNmeaDecimals(const NmeaDecimals& d);
[[nodiscard]] Command GetNmeaDecimals();
[[nodiscard]] std::optional<Command> SetRtk(const RtkSettings& s);
[[nodiscard]] Command GetRtk();
[[nodiscard]] Command SetConstellations(const Constellations& c);
[[nodiscard]] Command GetConstellations();
[[nodiscard]] std::optional<Command> SetElevationMask(double deg);
[[nodiscard]] Command GetElevationMask();
[[nodiscard]] std::optional<Command> SetCnrMask(double dbhz);
[[nodiscard]] Command GetCnrMask();
[[nodiscard]] Command SetReceiverMode(Mode m);
[[nodiscard]] Command GetReceiverMode();
/// Base survey-in: average at least `seconds` positions; stop early once the
/// 3-D accuracy is below `accuracy_m` (0 = no limit).
[[nodiscard]] std::optional<Command> SurveyIn(int seconds, double accuracy_m);
/// Base at a fixed ECEF position (metres, in the frame the rovers should get).
[[nodiscard]] Command FixedBase(double x_m, double y_m, double z_m);
/// Survey-in off.
[[nodiscard]] Command DisableSurvey();
[[nodiscard]] Command GetSurvey();
[[nodiscard]] std::optional<Command> SetPps(const PpsSettings& p);
[[nodiscard]] Command GetPps();
/// UART `port` (1..3) baud; the host must reopen at the new baud afterwards.
[[nodiscard]] std::optional<Command> SetUartBaud(int port, int baud);
[[nodiscard]] Command SaveParams();
[[nodiscard]] Command SoftwareReset();
[[nodiscard]] Command Restart(Start s);
/// Factory defaults (takes effect after a reset).
[[nodiscard]] Command RestoreDefaults();

/// Get commands for every setting `want` manages.
[[nodiscard]] std::vector<Command> ReadCommands(const ReceiverConfig& want);
/// Stores the values of a Get reply in `have`; ignores other replies.
void ApplyReadReply(const nmea::OwnedReply& reply, ReceiverConfig& have);
/// Writes needed to get from `have` to `want` (unknown current values are
/// written), followed by SaveParams when anything changed and SoftwareReset
/// when a changed setting needs one. Empty when nothing differs.
[[nodiscard]] std::vector<Command> PlanConfig(const ReceiverConfig& want,
                                              const ReceiverConfig& have);

}  // namespace lgrtk::lg290p

#endif  // LGRTK_LG290P_H_
