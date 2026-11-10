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
  /// Field-wise equality.
  bool operator==(const MessageRate&) const = default;
};
/// NMEA decimal places (PQTMCFGNMEADP).
struct NmeaDecimals {
  int utc = 3;  ///< UTC time decimals
  int pos = 8;  ///< latitude/longitude minute decimals (8 = 0.02 mm)
  int alt = 3;  ///< altitude decimals
  int dop = 2;  ///< DOP decimals
  int spd = 3;  ///< speed decimals
  int cog = 2;  ///< course decimals
  /// Field-wise equality.
  bool operator==(const NmeaDecimals&) const = default;
};
/// RTK engine settings (PQTMCFGRTK).
struct RtkSettings {
  int diff_mode = 1;    ///< 0 off, 1 auto (RTK), 2 RTD only
  int rel_mode = 1;     ///< 1 absolute, 2 relative
  int timeout_s = 120;  ///< max differential age for an RTK fix, 1..600
  /// Field-wise equality.
  bool operator==(const RtkSettings&) const = default;
};
/// Enabled constellations (PQTMCFGCNST).
struct Constellations {
  bool gps = true;      ///< GPS
  bool glonass = true;  ///< GLONASS
  bool galileo = true;  ///< Galileo
  bool beidou = true;   ///< BeiDou
  bool qzss = true;     ///< QZSS
  bool navic = true;    ///< NavIC
  /// Field-wise equality.
  bool operator==(const Constellations&) const = default;
};
/// PPS output (PQTMCFGPPS, index 1).
struct PpsSettings {
  bool enable = true;     ///< output on
  int duration_ms = 100;  ///< pulse width 0..900
  int mode = 1;           ///< 1 always, 2 only with a fix
  int polarity = 1;       ///< 1 positive
  /// Field-wise equality.
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

/// `$PQTMVERNO`: firmware version string, build date and time.
[[nodiscard]] Command QueryVersion();
/// `$PQTMUNIQID`: the module's unique id.
[[nodiscard]] Command QueryUniqueId();
/// `$PQTMCFGFIXRATE,W`: fix interval 50..1000 ms (needs save + reset).
[[nodiscard]] std::optional<Command> SetFixRate(int interval_ms);
/// `$PQTMCFGFIXRATE,R`.
[[nodiscard]] Command GetFixRate();
/// `$PQTMCFGMSGRATE,W`: output `msg` once every `rate` fixes (0 = off);
/// `version` for PQTM messages (e.g. PQTMEPE 2) or the RTCM offset.
[[nodiscard]] Command SetMessageRate(std::string_view msg, int rate,
                                     int version = 0);
/// `$PQTMCFGMSGRATE,R` for one message.
[[nodiscard]] Command GetMessageRate(std::string_view msg, int version = 0);
/// `$PQTMCFGNMEADP,W`: NMEA decimal places.
[[nodiscard]] Command SetNmeaDecimals(const NmeaDecimals& d);
/// `$PQTMCFGNMEADP,R`.
[[nodiscard]] Command GetNmeaDecimals();
/// `$PQTMCFGRTK,W`: RTK mode and maximum differential age.
[[nodiscard]] std::optional<Command> SetRtk(const RtkSettings& s);
/// `$PQTMCFGRTK,R`.
[[nodiscard]] Command GetRtk();
/// `$PQTMCFGCNST,W` (needs save + reset).
[[nodiscard]] Command SetConstellations(const Constellations& c);
/// `$PQTMCFGCNST,R`.
[[nodiscard]] Command GetConstellations();
/// `$PQTMCFGELETHD,W`: elevation mask 0..90 deg.
[[nodiscard]] std::optional<Command> SetElevationMask(double deg);
/// `$PQTMCFGELETHD,R`.
[[nodiscard]] Command GetElevationMask();
/// `$PQTMCFGCNRTHD,W`: C/N0 mask 0..99 dBHz.
[[nodiscard]] std::optional<Command> SetCnrMask(double dbhz);
/// `$PQTMCFGCNRTHD,R`.
[[nodiscard]] Command GetCnrMask();
/// `$PQTMCFGRCVRMODE,W`: rover or base (needs save + reset).
[[nodiscard]] Command SetReceiverMode(Mode m);
/// `$PQTMCFGRCVRMODE,R`.
[[nodiscard]] Command GetReceiverMode();
/// Base survey-in: average at least `seconds` positions; stop early once the
/// 3-D accuracy is below `accuracy_m` (0 = no limit).
[[nodiscard]] std::optional<Command> SurveyIn(int seconds, double accuracy_m);
/// Base at a fixed ECEF position (metres, in the frame the rovers should get).
[[nodiscard]] Command FixedBase(double x_m, double y_m, double z_m);
/// Survey-in off.
[[nodiscard]] Command DisableSurvey();
/// `$PQTMCFGSVIN,R`.
[[nodiscard]] Command GetSurvey();
/// `$PQTMCFGPPS,W` for PPS index 1.
[[nodiscard]] std::optional<Command> SetPps(const PpsSettings& p);
/// `$PQTMCFGPPS,R,1`.
[[nodiscard]] Command GetPps();
/// UART `port` (1..3) baud; the host must reopen at the new baud afterwards.
[[nodiscard]] std::optional<Command> SetUartBaud(int port, int baud);
/// `$PQTMSAVEPAR`: save the configuration to flash.
[[nodiscard]] Command SaveParams();
/// `$PQTMSRR`: reboot (no reply).
[[nodiscard]] Command SoftwareReset();
/// `$PQTMHOT` / `$PQTMWARM` / `$PQTMCOLD` (no reply).
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
