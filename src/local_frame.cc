// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/local_frame.h"

#include <cmath>
#include <numbers>

namespace lgrtk {

namespace {

constexpr double kA = 6378137.0;            // GRS80 semi-major axis
constexpr double kF = 1.0 / 298.257222101;  // GRS80 flattening
constexpr double kE2 = kF * (2.0 - kF);     // first eccentricity squared
constexpr double kDeg = std::numbers::pi / 180.0;

}  // namespace

void LocalFrame::GeodeticToEcef(double lat_deg, double lon_deg, double h_m,
                                double xyz[3]) noexcept {
  const double lat = lat_deg * kDeg;
  const double lon = lon_deg * kDeg;
  const double s = std::sin(lat);
  const double n = kA / std::sqrt(1.0 - kE2 * s * s);
  xyz[0] = (n + h_m) * std::cos(lat) * std::cos(lon);
  xyz[1] = (n + h_m) * std::cos(lat) * std::sin(lon);
  xyz[2] = (n * (1.0 - kE2) + h_m) * s;
}

void LocalFrame::EcefToGeodetic(const double xyz[3], double& lat_deg,
                                double& lon_deg, double& h_m) noexcept {
  const double p = std::hypot(xyz[0], xyz[1]);
  double lat = std::atan2(xyz[2], p * (1.0 - kE2));
  double h = 0.0;
  for (int i = 0; i < 6; ++i) {
    const double s = std::sin(lat);
    const double n = kA / std::sqrt(1.0 - kE2 * s * s);
    h = p / std::cos(lat) - n;
    lat = std::atan2(xyz[2], p * (1.0 - kE2 * n / (n + h)));
  }
  lat_deg = lat / kDeg;
  lon_deg = std::atan2(xyz[1], xyz[0]) / kDeg;
  h_m = h;
}

LocalFrame::LocalFrame(double lat0_deg, double lon0_deg, double h0_m) noexcept {
  GeodeticToEcef(lat0_deg, lon0_deg, h0_m, origin_);
  const double sl = std::sin(lat0_deg * kDeg);
  const double cl = std::cos(lat0_deg * kDeg);
  const double so = std::sin(lon0_deg * kDeg);
  const double co = std::cos(lon0_deg * kDeg);
  const double r[3][3] = {
      {-so, co, 0.0}, {-sl * co, -sl * so, cl}, {cl * co, cl * so, sl}};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      r_[i][j] = r[i][j];
    }
  }
}

void LocalFrame::ToEnu(double lat_deg, double lon_deg, double h_m,
                       double enu[3]) const noexcept {
  double x[3];
  GeodeticToEcef(lat_deg, lon_deg, h_m, x);
  const double d[3] = {x[0] - origin_[0], x[1] - origin_[1], x[2] - origin_[2]};
  for (int i = 0; i < 3; ++i) {
    enu[i] = r_[i][0] * d[0] + r_[i][1] * d[1] + r_[i][2] * d[2];
  }
}

void LocalFrame::FromEnu(const double enu[3], double& lat_deg, double& lon_deg,
                         double& h_m) const noexcept {
  double x[3];
  for (int j = 0; j < 3; ++j) {
    x[j] =
        origin_[j] + r_[0][j] * enu[0] + r_[1][j] * enu[1] + r_[2][j] * enu[2];
  }
  EcefToGeodetic(x, lat_deg, lon_deg, h_m);
}

}  // namespace lgrtk
