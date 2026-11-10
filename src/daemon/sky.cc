// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/sky.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#include <nlohmann/json.hpp>

#include "daemon/gpsd_json.h"
#include "lgrtk/nmea.h"

namespace lgrtk::daemon {
namespace {

constexpr int kGps = 0;
constexpr int kSbas = 1;
constexpr int kGalileo = 2;
constexpr int kBeidou = 3;
constexpr int kQzss = 5;
constexpr int kGlonass = 6;
constexpr int kNavic = 7;

// gpsd gnssid of a talker; -1 for GN/unknown (GSV is never multi-talker).
int GnssOfTalker(nmea::Talker t) {
  switch (t) {
    case nmea::Talker::kGps:
      return kGps;
    case nmea::Talker::kGlonass:
      return kGlonass;
    case nmea::Talker::kGalileo:
      return kGalileo;
    case nmea::Talker::kBeidou:
      return kBeidou;
    case nmea::Talker::kQzss:
      return kQzss;
    case nmea::Talker::kNavic:
      return kNavic;
    default:
      return -1;
  }
}

// gpsd gnssid of an NMEA 4.10 GSA system id.
int GnssOfSystemId(int id) {
  switch (id) {
    case 1:
      return kGps;
    case 2:
      return kGlonass;
    case 3:
      return kGalileo;
    case 4:
      return kBeidou;
    case 5:
      return kQzss;
    case 6:
      return kNavic;
    default:
      return -1;
  }
}

// GPS-talker PRNs 33..64 are SBAS (NMEA convention).
int GnssOfSat(int talker_gnss, uint16_t prn) {
  return talker_gnss == kGps && prn >= 33 && prn <= 64 ? kSbas : talker_gnss;
}

double Round2(double v) { return std::round(v * 100.0) / 100.0; }

}  // namespace

void SkyTracker::FeedNmea(std::string_view line) {
  const std::string_view body = nmea::Validate(line);
  if (body.empty()) {
    return;
  }
  const std::string_view type = nmea::SentenceType(body);
  if (type == "GSV") {
    FeedGsv(line);
  } else if (type == "GSA") {
    FeedGsa(line);
  }
}

void SkyTracker::FeedGsv(std::string_view line) {
  const auto g = nmea::ParseGsv(line);
  if (!g) {
    return;
  }
  const int gnss = GnssOfTalker(g->talker);
  if (gnss < 0 || g->msg_num == 0 || g->msg_num > g->total_msgs) {
    return;
  }
  const GroupKey key{gnss, g->signal_id};
  auto& part = partial_[key];
  if (g->msg_num == 1) {
    part.clear();
  }
  for (size_t i = 0; i < g->count; ++i) {
    const auto& s = g->sats[i];
    part.push_back({s.prn, s.elev_deg, s.az_deg, s.cn0_dbhz});
  }
  if (g->msg_num == g->total_msgs) {
    groups_[key] = std::move(part);
    partial_.erase(key);
  }
}

void SkyTracker::FeedGsa(std::string_view line) {
  const auto gsa = nmea::ParseGsa(line);
  if (!gsa) {
    return;
  }
  hdop_ = static_cast<float>(gsa->hdop);
  pdop_ = static_cast<float>(gsa->pdop);
  const int gnss = GnssOfSystemId(gsa->system_id);
  if (gnss < 0) {
    return;
  }
  std::array<std::string_view, nmea::kMaxFields> f{};
  const size_t n = nmea::SplitFields(nmea::Validate(line), f);
  auto& used = used_[gnss];
  used.clear();
  // Fields 3..14 hold up to 12 PRNs.
  for (size_t i = 3; i < std::min<size_t>(n, 15); ++i) {
    unsigned prn = 0;
    const auto* b = f[i].data();
    const auto* e = b + f[i].size();
    if (!f[i].empty() && std::from_chars(b, e, prn).ec == std::errc() &&
        prn <= 0xFFFF) {
      used.insert(static_cast<uint16_t>(prn));
    }
  }
}

std::vector<SkySat> SkyTracker::Satellites() const {
  std::map<std::pair<int, uint16_t>, SkySat> merged;
  for (const auto& [key, sats] : groups_) {
    for (const auto& s : sats) {
      const int gnss = GnssOfSat(key.first, s.prn);
      auto [it, inserted] = merged.try_emplace(
          {gnss, s.prn}, SkySat{s.prn, s.elevation_deg, s.azimuth_deg,
                                s.cn0_dbhz, false, gnss});
      if (!inserted) {
        it->second.cn0_dbhz = std::max(it->second.cn0_dbhz, s.cn0_dbhz);
        if (it->second.elevation_deg < 0) {
          it->second.elevation_deg = s.elevation_deg;
          it->second.azimuth_deg = s.azimuth_deg;
        }
      }
    }
  }
  std::vector<SkySat> out;
  out.reserve(merged.size());
  for (auto& [key, s] : merged) {
    // SBAS satellites appear in the GPS system's GSA list.
    const int used_system = s.gnssid == kSbas ? kGps : s.gnssid;
    auto it = used_.find(used_system);
    s.used = it != used_.end() && it->second.contains(s.prn);
    out.push_back(s);
  }
  return out;
}

std::string SkyJson(const SkyTracker& sky, const std::string& device,
                    std::optional<int64_t> unix_ns) {
  nlohmann::ordered_json o;
  o["class"] = "SKY";
  o["device"] = device;
  if (unix_ns) {
    o["time"] = IsoTime(*unix_ns);
  }
  o["hdop"] = Round2(sky.Hdop());
  o["pdop"] = Round2(sky.Pdop());
  const auto sats = sky.Satellites();
  int used = 0;
  auto list = nlohmann::ordered_json::array();
  for (const auto& s : sats) {
    nlohmann::ordered_json j;
    j["PRN"] = s.prn;
    j["gnssid"] = s.gnssid;
    j["svid"] = s.prn;
    if (s.elevation_deg >= 0) {
      j["el"] = s.elevation_deg;
      j["az"] = s.azimuth_deg;
    }
    if (s.cn0_dbhz >= 0) {
      j["ss"] = s.cn0_dbhz;
    }
    j["used"] = s.used;
    used += s.used ? 1 : 0;
    list.push_back(std::move(j));
  }
  o["nSat"] = sats.size();
  o["uSat"] = used;
  o["satellites"] = std::move(list);
  return o.dump() + "\r\n";
}

}  // namespace lgrtk::daemon
