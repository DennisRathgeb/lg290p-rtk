// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/lg290p.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace lgrtk::lg290p {

namespace {

Command Make(std::string body, std::string reply_to, bool is_write, bool needs_reset = false,
             std::string match_value = {}) {
  Command c;
  c.text = BuildCommand(body);
  c.reply_to = std::move(reply_to);
  c.is_write = is_write;
  c.needs_reset = needs_reset;
  c.match_value = std::move(match_value);
  return c;
}

std::string Fmt1(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", v);
  return buf;
}

std::string Fmt4(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.4f", v);
  return buf;
}

std::string MsgBody(char rw, std::string_view msg, int rate, int version) {
  std::string b = "PQTMCFGMSGRATE,";
  b += rw;
  b += ',';
  b += msg;
  if (rw == 'W') b += "," + std::to_string(rate);
  if (version != 0) b += "," + std::to_string(version);
  return b;
}

bool ToInt(const std::string& s, int& out) {
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool ToDouble(const std::string& s, double& out) {
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool SameTenth(const std::optional<double>& a, const std::optional<double>& b) {
  return a && b && std::lround(*a * 10.0) == std::lround(*b * 10.0);
}

constexpr int kBauds[] = {9600, 115200, 230400, 460800, 921600, 3000000};

}  // namespace

std::string BuildCommand(std::string_view body) {
  unsigned sum = 0;
  for (char c : body) sum ^= static_cast<unsigned char>(c);
  char tail[8];
  std::snprintf(tail, sizeof tail, "*%02X\r\n", sum & 0xFF);
  std::string s = "$";
  s += body;
  s += tail;
  return s;
}

bool Matches(const Command& cmd, const nmea::Reply& reply) noexcept {
  if (cmd.reply_to.empty() || reply.command != cmd.reply_to) return false;
  if (!reply.ok || cmd.match_value.empty() || reply.values.empty()) return true;
  return reply.values[0] == cmd.match_value;
}

bool Matches(const Command& cmd, const nmea::OwnedReply& reply) noexcept {
  if (cmd.reply_to.empty() || reply.command != cmd.reply_to) return false;
  if (!reply.ok || cmd.match_value.empty() || reply.values.empty()) return true;
  return reply.values[0] == cmd.match_value;
}

ReceiverConfig DefaultRoverConfig() {
  ReceiverConfig c;
  c.fix_interval_ms = 100;
  c.nmea_decimals = NmeaDecimals{};
  c.message_rates = {{"GGA", 1, 0}, {"RMC", 1, 0}, {"GST", 1, 0}, {"PQTMEPE", 1, 2},
                     {"GLL", 0, 0}, {"VTG", 0, 0}};
  c.rtk = RtkSettings{1, 1, 30};
  return c;
}

Command QueryVersion() { return Make("PQTMVERNO", "PQTMVERNO", false); }
Command QueryUniqueId() { return Make("PQTMUNIQID", "PQTMUNIQID", false); }

std::optional<Command> SetFixRate(int interval_ms) {
  if (interval_ms < 50 || interval_ms > 1000) return std::nullopt;
  return Make("PQTMCFGFIXRATE,W," + std::to_string(interval_ms), "PQTMCFGFIXRATE", true, true);
}
Command GetFixRate() { return Make("PQTMCFGFIXRATE,R", "PQTMCFGFIXRATE", false); }

Command SetMessageRate(std::string_view msg, int rate, int version) {
  return Make(MsgBody('W', msg, std::clamp(rate, 0, 255), version), "PQTMCFGMSGRATE", true);
}
Command GetMessageRate(std::string_view msg, int version) {
  return Make(MsgBody('R', msg, 0, version), "PQTMCFGMSGRATE", false, false, std::string(msg));
}

Command SetNmeaDecimals(const NmeaDecimals& d) {
  return Make("PQTMCFGNMEADP,W," + std::to_string(d.utc) + "," + std::to_string(d.pos) + "," +
                  std::to_string(d.alt) + "," + std::to_string(d.dop) + "," +
                  std::to_string(d.spd) + "," + std::to_string(d.cog),
              "PQTMCFGNMEADP", true);
}
Command GetNmeaDecimals() { return Make("PQTMCFGNMEADP,R", "PQTMCFGNMEADP", false); }

std::optional<Command> SetRtk(const RtkSettings& s) {
  if (s.diff_mode < 0 || s.diff_mode > 2 || s.rel_mode < 1 || s.rel_mode > 2 || s.timeout_s < 1 ||
      s.timeout_s > 600) {
    return std::nullopt;
  }
  return Make("PQTMCFGRTK,W," + std::to_string(s.diff_mode) + "," + std::to_string(s.rel_mode) +
                  "," + std::to_string(s.timeout_s),
              "PQTMCFGRTK", true);
}
Command GetRtk() { return Make("PQTMCFGRTK,R", "PQTMCFGRTK", false); }

Command SetConstellations(const Constellations& c) {
  auto b = [](bool v) { return v ? std::string("1") : std::string("0"); };
  return Make("PQTMCFGCNST,W," + b(c.gps) + "," + b(c.glonass) + "," + b(c.galileo) + "," +
                  b(c.beidou) + "," + b(c.qzss) + "," + b(c.navic),
              "PQTMCFGCNST", true, true);
}
Command GetConstellations() { return Make("PQTMCFGCNST,R", "PQTMCFGCNST", false); }

std::optional<Command> SetElevationMask(double deg) {
  if (!(deg >= 0.0 && deg <= 90.0)) return std::nullopt;
  return Make("PQTMCFGELETHD,W," + Fmt1(deg), "PQTMCFGELETHD", true);
}
Command GetElevationMask() { return Make("PQTMCFGELETHD,R", "PQTMCFGELETHD", false); }

std::optional<Command> SetCnrMask(double dbhz) {
  if (!(dbhz >= 0.0 && dbhz <= 99.0)) return std::nullopt;
  return Make("PQTMCFGCNRTHD,W," + Fmt1(dbhz), "PQTMCFGCNRTHD", true);
}
Command GetCnrMask() { return Make("PQTMCFGCNRTHD,R", "PQTMCFGCNRTHD", false); }

Command SetReceiverMode(Mode m) {
  return Make("PQTMCFGRCVRMODE,W," + std::to_string(static_cast<int>(m)), "PQTMCFGRCVRMODE", true,
              true);
}
Command GetReceiverMode() { return Make("PQTMCFGRCVRMODE,R", "PQTMCFGRCVRMODE", false); }

std::optional<Command> SurveyIn(int seconds, double accuracy_m) {
  if (seconds < 1 || !(accuracy_m >= 0.0)) return std::nullopt;
  return Make("PQTMCFGSVIN,W,1," + std::to_string(seconds) + "," + Fmt1(accuracy_m) + ",0,0,0",
              "PQTMCFGSVIN", true, true);
}
Command FixedBase(double x_m, double y_m, double z_m) {
  return Make("PQTMCFGSVIN,W,2,0,0," + Fmt4(x_m) + "," + Fmt4(y_m) + "," + Fmt4(z_m),
              "PQTMCFGSVIN", true, true);
}
Command DisableSurvey() { return Make("PQTMCFGSVIN,W,0,0,0,0,0,0", "PQTMCFGSVIN", true, true); }
Command GetSurvey() { return Make("PQTMCFGSVIN,R", "PQTMCFGSVIN", false); }

std::optional<Command> SetPps(const PpsSettings& p) {
  if (p.duration_ms < 0 || p.duration_ms > 900 || p.mode < 1 || p.mode > 2 || p.polarity < 0 ||
      p.polarity > 1) {
    return std::nullopt;
  }
  return Make("PQTMCFGPPS,W,1," + std::string(p.enable ? "1" : "0") + "," +
                  std::to_string(p.duration_ms) + "," + std::to_string(p.mode) + "," +
                  std::to_string(p.polarity) + ",0",
              "PQTMCFGPPS", true);
}
Command GetPps() { return Make("PQTMCFGPPS,R,1", "PQTMCFGPPS", false); }

std::optional<Command> SetUartBaud(int port, int baud) {
  if (port < 1 || port > 3 || std::find(std::begin(kBauds), std::end(kBauds), baud) == std::end(kBauds)) {
    return std::nullopt;
  }
  return Make("PQTMCFGUART,W," + std::to_string(port) + "," + std::to_string(baud), "PQTMCFGUART",
              true, true);
}

Command SaveParams() { return Make("PQTMSAVEPAR", "PQTMSAVEPAR", true); }
Command SoftwareReset() { return Make("PQTMSRR", "", true); }
Command Restart(Start s) {
  switch (s) {
    case Start::kHot:
      return Make("PQTMHOT", "", true);
    case Start::kWarm:
      return Make("PQTMWARM", "", true);
    case Start::kCold:
      return Make("PQTMCOLD", "", true);
  }
  return Make("PQTMHOT", "", true);
}
Command RestoreDefaults() { return Make("PQTMRESTOREPAR", "PQTMRESTOREPAR", true, true); }

std::vector<Command> ReadCommands(const ReceiverConfig& want) {
  std::vector<Command> out;
  if (want.fix_interval_ms) out.push_back(GetFixRate());
  for (const auto& m : want.message_rates) out.push_back(GetMessageRate(m.name, m.version));
  if (want.nmea_decimals) out.push_back(GetNmeaDecimals());
  if (want.rtk) out.push_back(GetRtk());
  if (want.constellations) out.push_back(GetConstellations());
  if (want.elevation_mask_deg) out.push_back(GetElevationMask());
  if (want.cnr_mask_dbhz) out.push_back(GetCnrMask());
  if (want.receiver_mode) out.push_back(GetReceiverMode());
  if (want.pps) out.push_back(GetPps());
  return out;
}

void ApplyReadReply(const nmea::OwnedReply& r, ReceiverConfig& have) {
  if (!r.ok || r.values.empty()) return;
  const auto& v = r.values;
  std::vector<int> n(v.size(), 0);
  std::vector<bool> is_int(v.size(), false);
  for (size_t i = 0; i < v.size(); ++i) is_int[i] = ToInt(v[i], n[i]);
  auto all_int = [&](size_t count, size_t from = 0) {
    if (v.size() < from + count) return false;
    for (size_t i = from; i < from + count; ++i) {
      if (!is_int[i]) return false;
    }
    return true;
  };
  const std::string& c = r.command;
  if (c == "PQTMCFGFIXRATE" && all_int(1)) {
    have.fix_interval_ms = n[0];
  } else if (c == "PQTMCFGMSGRATE" && v.size() >= 2 && is_int[1]) {
    MessageRate m{v[0], n[1], v.size() >= 3 && is_int[2] ? n[2] : 0};
    auto it = std::find_if(have.message_rates.begin(), have.message_rates.end(),
                           [&](const MessageRate& x) { return x.name == m.name; });
    if (it != have.message_rates.end()) {
      *it = m;
    } else {
      have.message_rates.push_back(m);
    }
  } else if (c == "PQTMCFGNMEADP" && all_int(6)) {
    have.nmea_decimals = NmeaDecimals{n[0], n[1], n[2], n[3], n[4], n[5]};
  } else if (c == "PQTMCFGRTK" && all_int(3)) {
    have.rtk = RtkSettings{n[0], n[1], n[2]};
  } else if (c == "PQTMCFGCNST" && all_int(6)) {
    have.constellations = Constellations{n[0] != 0, n[1] != 0, n[2] != 0,
                                         n[3] != 0, n[4] != 0, n[5] != 0};
  } else if (c == "PQTMCFGELETHD") {
    double d = 0;
    if (ToDouble(v[0], d)) have.elevation_mask_deg = d;
  } else if (c == "PQTMCFGCNRTHD") {
    double d = 0;
    if (ToDouble(v[0], d)) have.cnr_mask_dbhz = d;
  } else if (c == "PQTMCFGRCVRMODE" && all_int(1) && (n[0] == 1 || n[0] == 2)) {
    have.receiver_mode = static_cast<Mode>(n[0]);
  } else if (c == "PQTMCFGPPS" && all_int(5)) {
    have.pps = PpsSettings{n[1] != 0, n[2], n[3], n[4]};
  }
}

std::vector<Command> PlanConfig(const ReceiverConfig& want, const ReceiverConfig& have) {
  std::vector<Command> out;
  if (want.fix_interval_ms && want.fix_interval_ms != have.fix_interval_ms) {
    if (auto c = SetFixRate(*want.fix_interval_ms)) out.push_back(*c);
  }
  for (const auto& m : want.message_rates) {
    const auto it = std::find_if(have.message_rates.begin(), have.message_rates.end(),
                                 [&](const MessageRate& x) { return x.name == m.name; });
    if (it == have.message_rates.end() || it->rate != m.rate) {
      out.push_back(SetMessageRate(m.name, m.rate, m.version));
    }
  }
  if (want.nmea_decimals && want.nmea_decimals != have.nmea_decimals) {
    out.push_back(SetNmeaDecimals(*want.nmea_decimals));
  }
  if (want.rtk && want.rtk != have.rtk) {
    if (auto c = SetRtk(*want.rtk)) out.push_back(*c);
  }
  if (want.constellations && want.constellations != have.constellations) {
    out.push_back(SetConstellations(*want.constellations));
  }
  if (want.elevation_mask_deg && !SameTenth(want.elevation_mask_deg, have.elevation_mask_deg)) {
    if (auto c = SetElevationMask(*want.elevation_mask_deg)) out.push_back(*c);
  }
  if (want.cnr_mask_dbhz && !SameTenth(want.cnr_mask_dbhz, have.cnr_mask_dbhz)) {
    if (auto c = SetCnrMask(*want.cnr_mask_dbhz)) out.push_back(*c);
  }
  if (want.receiver_mode && want.receiver_mode != have.receiver_mode) {
    out.push_back(SetReceiverMode(*want.receiver_mode));
  }
  if (want.pps && want.pps != have.pps) {
    if (auto c = SetPps(*want.pps)) out.push_back(*c);
  }
  if (out.empty()) return out;
  const bool reset = std::any_of(out.begin(), out.end(), [](const Command& c) { return c.needs_reset; });
  out.push_back(SaveParams());
  if (reset) out.push_back(SoftwareReset());
  return out;
}

}  // namespace lgrtk::lg290p
