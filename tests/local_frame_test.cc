// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/local_frame.h"

#include <cmath>
#include <numbers>

#include <gtest/gtest.h>

namespace lgrtk {
namespace {

// Origin near Lai da Nalps: 46.63 N, 8.78 E.
constexpr double kLat = 46.63;
constexpr double kLon = 8.78;

TEST(LocalFrameTest, OriginIsZero) {
  const LocalFrame f(kLat, kLon, 2000.0);
  double enu[3];
  f.ToEnu(kLat, kLon, 2000.0, enu);
  EXPECT_NEAR(enu[0], 0.0, 1e-6);
  EXPECT_NEAR(enu[1], 0.0, 1e-6);
  EXPECT_NEAR(enu[2], 0.0, 1e-6);
}

TEST(LocalFrameTest, OneMetreEast) {
  const LocalFrame f(kLat, kLon, 2000.0);
  const double east[3] = {1.0, 0.0, 0.0};
  double lat = 0;
  double lon = 0;
  double h = 0;
  f.FromEnu(east, lat, lon, h);
  EXPECT_GT(lon, kLon);
  double enu[3];
  f.ToEnu(lat, lon, h, enu);
  EXPECT_NEAR(enu[0], 1.0, 1e-3);
  EXPECT_NEAR(enu[1], 0.0, 1e-3);
  EXPECT_NEAR(enu[2], 0.0, 1e-3);
  // 1 m of longitude at 46.63 deg is about 1 / (111320 * cos(lat)) deg.
  EXPECT_NEAR(
      (lon - kLon) * 111320.0 * std::cos(kLat * std::numbers::pi / 180.0), 1.0,
      0.01);
}

TEST(LocalFrameTest, RoundTripAtFiveKilometres) {
  const LocalFrame f(kLat, kLon, 2000.0);
  const double in[3] = {3500.0, -3500.0, 120.0};
  double lat = 0;
  double lon = 0;
  double h = 0;
  f.FromEnu(in, lat, lon, h);
  double out[3];
  f.ToEnu(lat, lon, h, out);
  for (int i = 0; i < 3; ++i) {
    EXPECT_NEAR(out[i], in[i], 1e-4);
  }
}

}  // namespace
}  // namespace lgrtk
