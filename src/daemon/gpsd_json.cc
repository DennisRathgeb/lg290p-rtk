// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/gpsd_json.h"

#include <cmath>
#include <cstdio>
#include <numbers>

#include "lgrtk/version.h"

namespace lgrtk::daemon {
namespace {

using Obj = nlohmann::ordered_json;

constexpr double kEp95 = 1.96;  // 1-sigma -> ~95 % for gpsd's ep* fields
constexpr int64_t kNsPerS = 1000000000;
constexpr int64_t kNsPerMs = 1000000;
constexpr int64_t kSPerDay = 86400;

std::string Line(const Obj& o) { return o.dump() + "\r\n"; }

// Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant).
int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void CivilFromDays(int64_t z, int64_t* y, int* m, int* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  *y = era * 400 + yoe + (*m <= 2 ? 1 : 0);
}

int GpsdStatus(FixType t) {
  switch (t) {
    case FixType::kRtkFixed:
      return 3;
    case FixType::kRtkFloat:
      return 4;
    case FixType::kDgnss:
      return 2;
    case FixType::kDeadReckoning:
      return 5;
    case FixType::kSimulation:
      return 8;
    default:
      return 1;
  }
}

double Round(double v, double scale) { return std::round(v * scale) / scale; }

void PutTime(const Fix& fix, Obj* o) {
  if (auto ns = FixUnixNs(fix)) {
    (*o)["time"] = IsoTime(*ns);
  }
}

}  // namespace

std::optional<int64_t> FixUnixNs(const Fix& fix) {
  if (fix.date_ymd <= 0) {
    return std::nullopt;
  }
  const int64_t y = fix.date_ymd / 10000;
  const int64_t m = (fix.date_ymd / 100) % 100;
  const int64_t d = fix.date_ymd % 100;
  if (m < 1 || m > 12 || d < 1 || d > 31 || !(fix.utc_s >= 0) ||
      fix.utc_s >= kSPerDay + 1) {
    return std::nullopt;
  }
  const auto ms = static_cast<int64_t>(std::llround(fix.utc_s * 1000.0));
  return DaysFromCivil(y, m, d) * kSPerDay * kNsPerS + ms * kNsPerMs;
}

std::string IsoTime(int64_t unix_ns) {
  int64_t days = unix_ns / (kSPerDay * kNsPerS);
  int64_t rem = unix_ns % (kSPerDay * kNsPerS);
  if (rem < 0) {
    rem += kSPerDay * kNsPerS;
    --days;
  }
  int64_t y = 0;
  int mo = 0;
  int d = 0;
  CivilFromDays(days, &y, &mo, &d);
  const int64_t ms = rem / kNsPerMs;
  char buf[48];
  std::snprintf(
      buf, sizeof buf, "%04lld-%02d-%02dT%02lld:%02lld:%02lld.%03lldZ",
      static_cast<long long>(y), mo, d, static_cast<long long>(ms / 3600000),
      static_cast<long long>((ms / 60000) % 60),
      static_cast<long long>((ms / 1000) % 60),
      static_cast<long long>(ms % 1000));
  return buf;
}

std::string VersionJson() {
  Obj o;
  o["class"] = "VERSION";
  o["release"] = std::string("lgrtkd ") + kVersion;
  o["rev"] = kVersion;
  o["proto_major"] = 3;
  o["proto_minor"] = 14;
  return Line(o);
}

std::string DevicesJson(const std::string& device, bool active, int baud) {
  Obj dev;
  dev["class"] = "DEVICE";
  dev["path"] = device;
  dev["driver"] = "Quectel LG290P";
  dev["activated"] = active ? 1 : 0;
  if (active && baud > 0) {
    dev["bps"] = baud;
  }
  dev["flags"] = 1;  // SEEN_GPS
  Obj o;
  o["class"] = "DEVICES";
  o["devices"] = Obj::array({dev});
  return Line(o);
}

std::string TpvJson(const Fix& fix, const std::string& device) {
  Obj o;
  o["class"] = "TPV";
  o["device"] = device;
  o["mode"] = fix.has_position ? 3 : 1;
  if (fix.has_position) {
    o["status"] = GpsdStatus(fix.type);
  }
  PutTime(fix, &o);
  if (!fix.has_position) {
    return Line(o);
  }
  o["lat"] = fix.lat_deg;
  o["lon"] = fix.lon_deg;
  o["altHAE"] = Round(fix.h_ell_m, 1e4);
  o["altMSL"] = Round(fix.h_ell_m - fix.geoid_sep_m, 1e4);
  o["geoidSep"] = Round(fix.geoid_sep_m, 1e4);
  if (fix.sigma_source != SigmaSource::kNone) {
    o["epx"] = Round(kEp95 * fix.sigma_e_m, 1e4);
    o["epy"] = Round(kEp95 * fix.sigma_n_m, 1e4);
    o["epv"] = Round(kEp95 * fix.sigma_u_m, 1e4);
  }
  if (fix.vel_valid) {
    const double e = fix.vel_e_mps;
    const double n = fix.vel_n_mps;
    double track = std::atan2(e, n) * 180.0 / std::numbers::pi;
    if (track < 0) {
      track += 360.0;
    }
    o["speed"] = Round(std::hypot(e, n), 1e3);
    o["track"] = Round(track, 1e4);
  }
  if (fix.diff_age_s > 0) {
    o["dgpsAge"] = Round(fix.diff_age_s, 1e3);
    o["dgpsSta"] = fix.station_id;
  }
  return Line(o);
}

std::string GstJson(const Fix& fix, const std::string& device) {
  if (fix.sigma_source == SigmaSource::kNone) {
    return "";
  }
  Obj o;
  o["class"] = "GST";
  o["device"] = device;
  PutTime(fix, &o);
  o["lat"] = Round(fix.sigma_n_m, 1e4);
  o["lon"] = Round(fix.sigma_e_m, 1e4);
  o["alt"] = Round(fix.sigma_u_m, 1e4);
  return Line(o);
}

std::string NoFixTpvJson(const std::string& device) {
  Obj o;
  o["class"] = "TPV";
  o["device"] = device;
  o["mode"] = 0;
  return Line(o);
}

std::string WatchJson(const Watch& w) {
  Obj o;
  o["class"] = "WATCH";
  o["enable"] = w.enable;
  o["json"] = w.json;
  o["nmea"] = w.nmea;
  o["raw"] = 0;
  o["scaled"] = false;
  o["timing"] = false;
  o["split24"] = false;
  o["pps"] = false;
  return Line(o);
}

void ApplyWatch(const nlohmann::json& arg, Watch* w) {
  if (!arg.is_object()) {
    return;
  }
  auto get = [&arg](const char* key, bool* out) {
    auto it = arg.find(key);
    if (it != arg.end() && it->is_boolean()) {
      *out = it->get<bool>();
    }
  };
  get("enable", &w->enable);
  get("json", &w->json);
  get("nmea", &w->nmea);
}

std::string ErrorJson(std::string_view message) {
  Obj o;
  o["class"] = "ERROR";
  o["message"] = std::string(message);
  return Line(o);
}

std::optional<GpsdCommand> ParseGpsdCommand(std::string_view line) {
  while (!line.empty() &&
         (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
    line.remove_suffix(1);
  }
  if (!line.empty() && line.back() == ';') {
    line.remove_suffix(1);
  }
  GpsdCommand c;
  std::string_view name = line;
  std::string_view arg;
  if (auto eq = line.find('='); eq != std::string_view::npos) {
    name = line.substr(0, eq);
    arg = line.substr(eq + 1);
  }
  if (name == "?VERSION") {
    c.kind = GpsdCommand::kVersion;
  } else if (name == "?WATCH") {
    c.kind = GpsdCommand::kWatch;
  } else if (name == "?POLL") {
    c.kind = GpsdCommand::kPoll;
  } else if (name == "?DEVICES") {
    c.kind = GpsdCommand::kDevices;
  } else {
    c.kind = GpsdCommand::kUnknown;
  }
  if (!arg.empty()) {
    c.arg = nlohmann::json::parse(arg, nullptr, /*allow_exceptions=*/false);
    if (!c.arg.is_object()) {
      return std::nullopt;
    }
  }
  return c;
}

}  // namespace lgrtk::daemon
