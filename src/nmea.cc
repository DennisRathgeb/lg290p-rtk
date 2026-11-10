// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/nmea.h"

#include <charconv>
#include <system_error>

#include "lgrtk/framer.h"

namespace lgrtk::nmea {

namespace {

constexpr double kKnotToMps = 1852.0 / 3600.0;

using Fields = std::array<std::string_view, kMaxFields>;

// Parses a decimal number; an empty field yields `empty_value`.
bool ParseDouble(std::string_view s, double& out, double empty_value = 0.0) noexcept {
  if (s.empty()) {
    out = empty_value;
    return true;
  }
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

template <class T>
bool ParseUnsigned(std::string_view s, T& out, T empty_value = 0) noexcept {
  if (s.empty()) {
    out = empty_value;
    return true;
  }
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool ParseInt(std::string_view s, int& out, int empty_value) noexcept {
  if (s.empty()) {
    out = empty_value;
    return true;
  }
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

// ddmm.mmmm (lat) or dddmm.mmmm (lon) with a hemisphere letter.
bool ParseAngle(std::string_view field, std::string_view hemi, size_t deg_digits,
                double& out_deg) noexcept {
  if (field.empty() && hemi.empty()) {
    out_deg = 0.0;
    return true;
  }
  if (field.size() < deg_digits + 2 || hemi.size() != 1) return false;
  unsigned deg = 0;
  double minutes = 0.0;
  if (!ParseUnsigned(field.substr(0, deg_digits), deg) ||
      !ParseDouble(field.substr(deg_digits), minutes)) {
    return false;
  }
  out_deg = static_cast<double>(deg) + minutes / 60.0;
  const char h = hemi[0];
  if (h == 'S' || h == 'W') out_deg = -out_deg;
  return h == 'N' || h == 'S' || h == 'E' || h == 'W';
}

// hhmmss.sss -> seconds of day.
bool ParseUtc(std::string_view s, double& out_s) noexcept {
  if (s.empty()) {
    out_s = 0.0;
    return true;
  }
  if (s.size() < 6) return false;
  unsigned hh = 0;
  unsigned mm = 0;
  double ss = 0.0;
  if (!ParseUnsigned(s.substr(0, 2), hh) || !ParseUnsigned(s.substr(2, 2), mm) ||
      !ParseDouble(s.substr(4), ss) || hh > 23 || mm > 59 || ss >= 61.0) {
    return false;
  }
  out_s = hh * 3600.0 + mm * 60.0 + ss;
  return true;
}

// Dates before 2000 are the receiver's "time unknown" default (1980-01-06).
int32_t MakeDate(unsigned y, unsigned m, unsigned d) noexcept {
  if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  return static_cast<int32_t>(y * 10000 + m * 100 + d);
}

std::string_view BodyOf(std::string_view line) noexcept {
  return line.starts_with('$') ? Validate(line) : line;
}

// Validates, splits and checks the sentence type; returns the field count or 0.
size_t Prepare(std::string_view line, std::string_view type, std::span<std::string_view> f) noexcept {
  const std::string_view body = BodyOf(line);
  if (body.empty() || SentenceType(body) != type) return 0;
  return SplitFields(body, f);
}

}  // namespace

std::string_view Validate(std::string_view line) noexcept {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
  if (!NmeaChecksumOk(line)) return {};
  return line.substr(1, line.size() - 4);
}

size_t SplitFields(std::string_view body, std::span<std::string_view> out) noexcept {
  size_t n = 0;
  size_t start = 0;
  while (n < out.size()) {
    const size_t comma = body.find(',', start);
    if (comma == std::string_view::npos) {
      out[n++] = body.substr(start);
      break;
    }
    out[n++] = body.substr(start, comma - start);
    start = comma + 1;
  }
  return n;
}

std::string_view SentenceType(std::string_view body) noexcept {
  const std::string_view head = body.substr(0, body.find(','));
  if (head.starts_with("PQTM")) return head;
  if (head.size() != 5 || head[0] == 'P') return {};
  return head.substr(2);
}

Talker TalkerOf(std::string_view body) noexcept {
  const std::string_view t = body.substr(0, 2);
  if (t == "GN") return Talker::kMulti;
  if (t == "GP") return Talker::kGps;
  if (t == "GL") return Talker::kGlonass;
  if (t == "GA") return Talker::kGalileo;
  if (t == "GB" || t == "BD") return Talker::kBeidou;
  if (t == "GQ") return Talker::kQzss;
  if (t == "GI") return Talker::kNavic;
  return Talker::kUnknown;
}

std::optional<Gga> ParseGga(std::string_view line) noexcept {
  Fields f{};
  if (Prepare(line, "GGA", f) < 15) return std::nullopt;
  Gga g{};
  unsigned quality = 0;
  unsigned sats = 0;
  unsigned station = 0;
  if (!ParseUtc(f[1], g.utc_s) || !ParseAngle(f[2], f[3], 2, g.lat_deg) ||
      !ParseAngle(f[4], f[5], 3, g.lon_deg) || !ParseUnsigned(f[6], quality) ||
      !ParseUnsigned(f[7], sats) || !ParseDouble(f[8], g.hdop) ||
      !ParseDouble(f[9], g.alt_msl_m) || !ParseDouble(f[11], g.geoid_sep_m) ||
      !ParseDouble(f[13], g.diff_age_s) || !ParseUnsigned(f[14], station) || quality > 9 ||
      sats > 255 || station > 0xFFFF) {
    return std::nullopt;
  }
  g.quality = static_cast<uint8_t>(quality);
  g.sats = static_cast<uint8_t>(sats);
  g.station_id = static_cast<uint16_t>(station);
  g.has_position = !f[2].empty() && !f[4].empty();
  g.has_diff_age = !f[13].empty();
  return g;
}

std::optional<Rmc> ParseRmc(std::string_view line) noexcept {
  Fields f{};
  if (Prepare(line, "RMC", f) < 10) return std::nullopt;
  Rmc r{};
  double knots = 0.0;
  if (!ParseUtc(f[1], r.utc_s) || f[2].size() > 1 || !ParseAngle(f[3], f[4], 2, r.lat_deg) ||
      !ParseAngle(f[5], f[6], 3, r.lon_deg) || !ParseDouble(f[7], knots) ||
      !ParseDouble(f[8], r.course_deg)) {
    return std::nullopt;
  }
  r.valid = f[2] == "A";
  r.has_course = !f[8].empty();
  r.speed_mps = knots * kKnotToMps;
  if (f[9].size() == 6) {
    unsigned d = 0;
    unsigned m = 0;
    unsigned y = 0;
    if (!ParseUnsigned(f[9].substr(0, 2), d) || !ParseUnsigned(f[9].substr(2, 2), m) ||
        !ParseUnsigned(f[9].substr(4, 2), y)) {
      return std::nullopt;
    }
    r.date_ymd = MakeDate(y < 80 ? 2000 + y : 1900 + y, m, d);
  }
  return r;
}

std::optional<Gst> ParseGst(std::string_view line) noexcept {
  Fields f{};
  if (Prepare(line, "GST", f) < 9) return std::nullopt;
  Gst g{};
  if (!ParseUtc(f[1], g.utc_s) || !ParseDouble(f[2], g.rms_m) || !ParseDouble(f[3], g.major_m) ||
      !ParseDouble(f[4], g.minor_m) || !ParseDouble(f[5], g.orient_deg) ||
      !ParseDouble(f[6], g.sigma_lat_m) || !ParseDouble(f[7], g.sigma_lon_m) ||
      !ParseDouble(f[8], g.sigma_alt_m)) {
    return std::nullopt;
  }
  g.valid = !f[6].empty() && !f[7].empty() && !f[8].empty();
  return g;
}

std::optional<Gsa> ParseGsa(std::string_view line) noexcept {
  Fields f{};
  const size_t n = Prepare(line, "GSA", f);
  if (n < 18) return std::nullopt;
  Gsa g{};
  unsigned mode = 0;
  unsigned system = 0;
  if (!ParseUnsigned(f[2], mode) || !ParseDouble(f[15], g.pdop) || !ParseDouble(f[16], g.hdop) ||
      !ParseDouble(f[17], g.vdop) || (n > 18 && !ParseUnsigned(f[18], system)) || mode > 3 ||
      system > 255) {
    return std::nullopt;
  }
  g.fix_mode = static_cast<uint8_t>(mode);
  g.system_id = static_cast<uint8_t>(system);
  for (size_t i = 3; i <= 14; ++i) {
    if (!f[i].empty()) ++g.used_count;
  }
  return g;
}

std::optional<Gsv> ParseGsv(std::string_view line) noexcept {
  Fields f{};
  const std::string_view body = BodyOf(line);
  const size_t n = Prepare(line, "GSV", f);
  if (n < 4) return std::nullopt;
  Gsv g{};
  g.talker = TalkerOf(body);
  unsigned total = 0;
  unsigned num = 0;
  unsigned in_view = 0;
  if (!ParseUnsigned(f[1], total) || !ParseUnsigned(f[2], num) || !ParseUnsigned(f[3], in_view) ||
      total > 255 || num > 255 || in_view > 255) {
    return std::nullopt;
  }
  g.total_msgs = static_cast<uint8_t>(total);
  g.msg_num = static_cast<uint8_t>(num);
  g.in_view = static_cast<uint8_t>(in_view);
  const size_t rest = n - 4;
  const size_t sats = std::min<size_t>(rest / 4, 4);
  if (rest % 4 == 1) {
    unsigned sig = 0;
    if (!ParseUnsigned(f[n - 1], sig) || sig > 255) return std::nullopt;
    g.signal_id = static_cast<uint8_t>(sig);
  }
  for (size_t i = 0; i < sats; ++i) {
    unsigned prn = 0;
    int elev = 0;
    int az = 0;
    int cn0 = 0;
    const size_t b = 4 + 4 * i;
    if (!ParseUnsigned(f[b], prn) || !ParseInt(f[b + 1], elev, -1) ||
        !ParseInt(f[b + 2], az, -1) || !ParseInt(f[b + 3], cn0, -1) || prn > 0xFFFF) {
      return std::nullopt;
    }
    g.sats[g.count++] = SatInView{static_cast<uint16_t>(prn), static_cast<int16_t>(elev),
                                  static_cast<int16_t>(az), static_cast<int16_t>(cn0)};
  }
  return g;
}

std::optional<Vtg> ParseVtg(std::string_view line) noexcept {
  Fields f{};
  if (Prepare(line, "VTG", f) < 9) return std::nullopt;
  Vtg v{};
  double kmh = 0.0;
  if (!ParseDouble(f[1], v.course_deg) || !ParseDouble(f[7], kmh)) return std::nullopt;
  v.speed_mps = kmh / 3.6;
  return v;
}

std::optional<Zda> ParseZda(std::string_view line) noexcept {
  Fields f{};
  if (Prepare(line, "ZDA", f) < 5) return std::nullopt;
  Zda z{};
  unsigned d = 0;
  unsigned m = 0;
  unsigned y = 0;
  if (!ParseUtc(f[1], z.utc_s) || !ParseUnsigned(f[2], d) || !ParseUnsigned(f[3], m) ||
      !ParseUnsigned(f[4], y)) {
    return std::nullopt;
  }
  z.date_ymd = MakeDate(y, m, d);
  return z;
}

std::optional<Epe> ParseEpe(std::string_view line) noexcept {
  Fields f{};
  if (Prepare(line, "PQTMEPE", f) < 7) return std::nullopt;
  Epe e{};
  if (!ParseDouble(f[2], e.north_m) || !ParseDouble(f[3], e.east_m) ||
      !ParseDouble(f[4], e.down_m) || !ParseDouble(f[5], e.h2d_m) || !ParseDouble(f[6], e.h3d_m)) {
    return std::nullopt;
  }
  return e;
}

std::optional<Verno> ParseVerno(std::string_view line) noexcept {
  Fields f{};
  if (Prepare(line, "PQTMVERNO", f) < 4 || f[1] == "ERROR" || f[1].empty()) return std::nullopt;
  return Verno{f[1], f[2], f[3]};
}

std::optional<SvinStatus> ParseSvinStatus(std::string_view line) noexcept {
  Fields f{};
  if (Prepare(line, "PQTMSVINSTATUS", f) < 12) return std::nullopt;
  SvinStatus s{};
  unsigned valid = 0;
  if (!ParseUnsigned(f[2], s.tow_ms) || !ParseUnsigned(f[3], valid) ||
      !ParseUnsigned(f[6], s.observations) || !ParseUnsigned(f[7], s.configured_duration) ||
      !ParseDouble(f[8], s.mean_x_m) || !ParseDouble(f[9], s.mean_y_m) ||
      !ParseDouble(f[10], s.mean_z_m) || !ParseDouble(f[11], s.mean_acc_m) || valid > 2) {
    return std::nullopt;
  }
  s.valid = static_cast<uint8_t>(valid);
  return s;
}

std::optional<Reply> ParseReply(std::string_view line, std::span<std::string_view> fields) noexcept {
  const std::string_view body = BodyOf(line);
  if (!body.starts_with("PQTM")) return std::nullopt;
  const size_t n = SplitFields(body, fields);
  if (n < 1) return std::nullopt;
  Reply r{};
  r.command = fields[0];
  if (n >= 2 && fields[1] == "OK") {
    r.ok = true;
    r.values = std::span<const std::string_view>(fields.data() + 2, n - 2);
    return r;
  }
  if (n >= 2 && fields[1] == "ERROR") {
    int code = 0;
    if (n >= 3 && !ParseInt(fields[2], code, 0)) return std::nullopt;
    r.ok = false;
    r.error_code = code;
    return r;
  }
  // PQTMVERNO answers its query without "OK".
  if (r.command == "PQTMVERNO" && n >= 2) {
    r.ok = true;
    r.values = std::span<const std::string_view>(fields.data() + 1, n - 1);
    return r;
  }
  return std::nullopt;
}

OwnedReply Own(const Reply& r) {
  OwnedReply o;
  o.command = std::string(r.command);
  o.ok = r.ok;
  o.error_code = r.error_code;
  o.values.reserve(r.values.size());
  for (std::string_view v : r.values) o.values.emplace_back(v);
  return o;
}

}  // namespace lgrtk::nmea
