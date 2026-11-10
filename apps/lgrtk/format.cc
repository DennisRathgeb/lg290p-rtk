// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "apps/lgrtk/format.h"

#include <cstdint>

#include "internal/clock.h"

namespace lgrtk::cli {
namespace {

constexpr double kNsPerSecond = static_cast<double>(internal::kNsPerS);

const char* SigmaSourceName(SigmaSource s) {
  switch (s) {
    case SigmaSource::kGst:
      return "gst";
    case SigmaSource::kEpe:
      return "epe";
    case SigmaSource::kNone:
      break;
  }
  return "-";
}

}  // namespace

std::string FormatFix(const Fix& f) {
  char buf[256];
  const double t = f.utc_s;
  std::snprintf(
      buf, sizeof buf,
      "%02d:%02d:%06.3f %-10s %13.9f %14.9f %9.3f  sigma %6.3f %6.3f %6.3f "
      "(%s)  sats %2u  "
      "hdop %4.1f  age %4.1f",
      static_cast<int>(t / 3600), static_cast<int>(t / 60) % 60,
      t - 60.0 * static_cast<int>(t / 60), ToString(f.type), f.lat_deg,
      f.lon_deg, f.h_ell_m, static_cast<double>(f.sigma_e_m),
      static_cast<double>(f.sigma_n_m), static_cast<double>(f.sigma_u_m),
      SigmaSourceName(f.sigma_source), f.sats, static_cast<double>(f.hdop),
      static_cast<double>(f.diff_age_s));
  return buf;
}

void PrintTransition(std::FILE* out, std::string_view what, const char* from,
                     const char* to, std::string_view reason) {
  const std::string label = what.empty() ? "" : std::string(what) + " ";
  std::fprintf(out, "[%s%s -> %s] %.*s\n", label.c_str(), from, to,
               static_cast<int>(reason.size()), reason.data());
  std::fflush(out);
}

void PrintEvent(std::FILE* out, std::string_view what, const StatusEvent& e) {
  PrintTransition(out, what, ToString(e.from), ToString(e.to), e.reason);
}

void ProbeStats::Add(std::string_view line) {
  const auto body = nmea::Validate(line);
  std::string type(nmea::SentenceType(body));
  if (type.empty()) {
    return;
  }
  ++types[type];
  if (type != "GGA") {
    return;
  }
  if (auto g = nmea::ParseGga(body)) {
    ++quality[g->quality];
    last_gga = g;
    if (first_utc_s < 0) {
      first_utc_s = g->utc_s;
    }
    if (last_utc_s >= 0 && g->utc_s < last_utc_s) {
      ++backwards;
    }
    last_utc_s = g->utc_s;
  }
}

void PrintProbeReport(const ReceiverStatus& st, const ProbeStats& stats,
                      double seconds) {
  std::printf("port: %s at %d baud\n", st.port.c_str(), st.baud);
  std::printf("firmware: %s\n", st.firmware.c_str());
  std::printf(
      "bytes %llu, NMEA %llu, RTCM %llu, bad checksum %llu, bad CRC %llu\n",
      static_cast<unsigned long long>(st.bytes),
      static_cast<unsigned long long>(st.framer.nmea),
      static_cast<unsigned long long>(st.framer.rtcm),
      static_cast<unsigned long long>(st.framer.bad_nmea),
      static_cast<unsigned long long>(st.framer.bad_rtcm));
  std::printf("sentences:");
  for (const auto& [t, n] : stats.types) {
    std::printf(" %s %.1f Hz", t.c_str(), n / seconds);
  }
  std::printf("\nGGA quality:");
  for (const auto& [q, n] : stats.quality) {
    std::printf(" %s x%d",
                ToString(FixTypeFromGgaQuality(static_cast<uint8_t>(q))), n);
  }
  if (const auto& g = stats.last_gga) {
    std::printf(
        "\nlast GGA: sats %u, hdop %.2f, diff age %s\n", g->sats, g->hdop,
        g->has_diff_age ? std::to_string(g->diff_age_s).c_str() : "none");
  } else {
    std::printf("\nlast GGA: none\n");
  }
  std::printf("GST: %s\n",
              stats.types.contains("GST")
                  ? "present"
                  : "not seen (enable: lgrtk config set msg-rate GST 1)");
  std::printf("PQTMEPE: %s\n",
              stats.types.contains("PQTMEPE") ? "present" : "not seen");
  std::printf(
      "UTC span %.1f s, backwards steps %d\n",
      stats.last_utc_s >= 0 ? stats.last_utc_s - stats.first_utc_s : 0.0,
      stats.backwards);
}

void PrintStatusReport(const ReceiverStatus& st) {
  std::printf(
      "state %s, firmware %s, baud %d, bytes %llu, epochs %llu, reconnects "
      "%llu, last fix age %.2f s\n",
      ToString(st.state), st.firmware.c_str(), st.baud,
      static_cast<unsigned long long>(st.bytes),
      static_cast<unsigned long long>(st.epochs),
      static_cast<unsigned long long>(st.reconnects),
      st.last_fix_age_ns < 0
          ? -1.0
          : static_cast<double>(st.last_fix_age_ns) / kNsPerSecond);
  if (!st.last_error.empty()) {
    std::printf("last error: %s\n", st.last_error.c_str());
  }
}

}  // namespace lgrtk::cli
