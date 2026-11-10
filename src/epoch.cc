// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/epoch.h"

#include <cmath>
#include <numbers>

#include "lgrtk/nmea.h"

namespace lgrtk {

const char* ToString(FixType t) noexcept {
  switch (t) {
    case FixType::kNone:
      return "none";
    case FixType::kSingle:
      return "single";
    case FixType::kDgnss:
      return "dgnss";
    case FixType::kPps:
      return "pps";
    case FixType::kRtkFixed:
      return "rtk-fixed";
    case FixType::kRtkFloat:
      return "rtk-float";
    case FixType::kDeadReckoning:
      return "dead-reckoning";
    case FixType::kManual:
      return "manual";
    case FixType::kSimulation:
      return "simulation";
  }
  return "?";
}

FixType FixTypeFromGgaQuality(uint8_t quality) noexcept {
  switch (quality) {
    case 1:
      return FixType::kSingle;
    case 2:
      return FixType::kDgnss;
    case 3:
      return FixType::kPps;
    case 4:
      return FixType::kRtkFixed;
    case 5:
      return FixType::kRtkFloat;
    case 6:
      return FixType::kDeadReckoning;
    case 7:
      return FixType::kManual;
    case 8:
      return FixType::kSimulation;
    default:
      return FixType::kNone;
  }
}

void EpochAssembler::StartEpoch(double utc_s) noexcept {
  pending_ = Fix{};
  pending_utc_ = utc_s;
  pending_.utc_s = utc_s;
  seen_ = 0;
  emitted_ = false;
  open_ = true;
}

bool EpochAssembler::Emit(Fix& out) noexcept {
  out = pending_;
  if (out.date_ymd == 0) out.date_ymd = last_date_;
  out.pdop = last_pdop_;
  out.seq = seq_++;
  emitted_ = true;
  return true;
}

bool EpochAssembler::Feed(std::string_view line, int64_t t_arrival_ns, Fix& out) noexcept {
  const std::string_view body = nmea::Validate(line);
  if (body.empty()) return false;
  const std::string_view type = nmea::SentenceType(body);

  uint8_t bit = 0;
  double utc = -1.0;
  std::optional<nmea::Gga> gga;
  std::optional<nmea::Rmc> rmc;
  std::optional<nmea::Gst> gst;
  std::optional<nmea::Epe> epe;
  std::optional<nmea::Zda> zda;
  if (type == "GGA") {
    if (!(gga = nmea::ParseGga(body))) return false;
    bit = kGga;
    utc = gga->utc_s;
  } else if (type == "RMC") {
    if (!(rmc = nmea::ParseRmc(body))) return false;
    bit = kRmc;
    utc = rmc->utc_s;
  } else if (type == "GST") {
    if (!(gst = nmea::ParseGst(body))) return false;
    bit = kGst;
    utc = gst->utc_s;
  } else if (type == "ZDA") {
    if (!(zda = nmea::ParseZda(body))) return false;
    bit = kZda;
    utc = zda->utc_s;
  } else if (type == "PQTMEPE") {
    if (!(epe = nmea::ParseEpe(body)) || !open_) return false;
    bit = kEpe;
  } else if (type == "GSA") {
    if (const auto gsa = nmea::ParseGsa(body)) last_pdop_ = static_cast<float>(gsa->pdop);
    return false;
  } else {
    return false;
  }

  bool closed = false;
  if (bit != kEpe) {
    if (open_ && utc != pending_utc_) {
      if (!emitted_ && (seen_ & kGga)) closed = Emit(out);
      expected_ = seen_;
      StartEpoch(utc);
    } else if (!open_) {
      StartEpoch(utc);
    }
  }

  Fix& p = pending_;
  if (gga) {
    p.type = FixTypeFromGgaQuality(gga->quality);
    p.has_position = gga->has_position;
    p.lat_deg = gga->lat_deg;
    p.lon_deg = gga->lon_deg;
    p.geoid_sep_m = gga->geoid_sep_m;
    p.h_ell_m = gga->alt_msl_m + gga->geoid_sep_m;
    p.hdop = static_cast<float>(gga->hdop);
    p.sats = gga->sats;
    p.diff_age_s = gga->has_diff_age ? static_cast<float>(gga->diff_age_s) : 0.0f;
    p.station_id = gga->station_id;
    const bool differential = p.type == FixType::kDgnss || p.type == FixType::kRtkFixed ||
                              p.type == FixType::kRtkFloat;
    p.frame = differential && gga->has_diff_age ? FixFrame::kCorrected : FixFrame::kAutonomous;
  } else if (rmc) {
    if (rmc->date_ymd != 0) p.date_ymd = last_date_ = rmc->date_ymd;
    if (rmc->valid && rmc->has_course) {
      const double c = rmc->course_deg * std::numbers::pi / 180.0;
      p.vel_e_mps = static_cast<float>(rmc->speed_mps * std::sin(c));
      p.vel_n_mps = static_cast<float>(rmc->speed_mps * std::cos(c));
      p.vel_u_mps = 0.0f;
      p.vel_valid = true;
    }
  } else if (gst) {
    if (gst->valid) {
      p.sigma_n_m = static_cast<float>(gst->sigma_lat_m);
      p.sigma_e_m = static_cast<float>(gst->sigma_lon_m);
      p.sigma_u_m = static_cast<float>(gst->sigma_alt_m);
      p.sigma_source = SigmaSource::kGst;
    }
  } else if (epe) {
    if (p.sigma_source != SigmaSource::kGst) {
      p.sigma_n_m = static_cast<float>(epe->north_m);
      p.sigma_e_m = static_cast<float>(epe->east_m);
      p.sigma_u_m = static_cast<float>(epe->down_m);
      p.sigma_source = SigmaSource::kEpe;
    }
  } else if (zda) {
    if (zda->date_ymd != 0) p.date_ymd = last_date_ = zda->date_ymd;
  }
  seen_ |= bit;
  p.t_arrival_ns = t_arrival_ns;

  if (closed) return true;
  if (!emitted_ && expected_ != 0 && (seen_ & kGga) && (seen_ & expected_) == expected_) {
    return Emit(out);
  }
  return false;
}

bool EpochAssembler::Flush(Fix& out) noexcept {
  const bool had = open_ && !emitted_ && (seen_ & kGga);
  if (had) Emit(out);
  open_ = false;
  return had;
}

void EpochAssembler::Reset() noexcept {
  open_ = false;
  emitted_ = false;
  seen_ = 0;
  expected_ = 0;
  pending_utc_ = -1.0;
}

}  // namespace lgrtk
