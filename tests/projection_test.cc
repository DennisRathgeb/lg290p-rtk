// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/projection.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>

namespace lgrtk {
namespace {

// Test point at Lai da Nalps (research note ntrip-and-swiss-coordinates.md,
// reference values from swisstopo REFRAME): ETRS89 46.63 N, 8.78 E.
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
  EXPECT_NEAR((lon - kLon) * 111320.0 * std::cos(kLat * M_PI / 180.0), 1.0, 0.01);
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
  for (int i = 0; i < 3; ++i) EXPECT_NEAR(out[i], in[i], 1e-4);
}

#ifdef LGRTK_HAVE_PROJ
TEST(ProjectionTest, Lv95AtNalps) {
  std::string err;
  auto p = Projection::Create("EPSG:4258", "EPSG:2056", err);
  ASSERT_TRUE(p) << err;
  double out[3];
  ASSERT_TRUE(p->Forward(kLat, kLon, 2000.0, out));
  EXPECT_NEAR(out[0], 2702718.1, 0.2);
  EXPECT_NEAR(out[1], 1165183.2, 0.2);
  double lat = 0;
  double lon = 0;
  double h = 0;
  ASSERT_TRUE(p->Inverse(out, lat, lon, h));
  EXPECT_NEAR(lat, kLat, 2e-8);  // ~2 mm: precision of the somerc inverse
  EXPECT_NEAR(lon, kLon, 2e-8);
  EXPECT_FALSE(p->Description().empty());
}

TEST(ProjectionTest, Lv95WithLhn95HeightsNeedsGeoidGrid) {
  std::string err;
  auto p = Projection::Create("EPSG:4937", "EPSG:2056+5729", err);
  if (!p) {
    // Without the swisstopo grid PROJ would fall back to a ballpark height:
    // refused, and the error names the grid to install.
    EXPECT_NE(err.find("chgeo2004"), std::string::npos) << err;
    GTEST_SKIP() << "ch_swisstopo grid not installed: " << err;
  }
  double out[3];
  ASSERT_TRUE(p->Forward(kLat, kLon, 2000.0, out));
  EXPECT_NEAR(out[2], 1948.830, 0.01);  // REFRAME: LHN95 height
}

TEST(ProjectionTest, RefusesUnknownCrs) {
  std::string err;
  EXPECT_FALSE(Projection::Create("EPSG:999999", "EPSG:2056", err));
  EXPECT_FALSE(err.empty());
}

TEST(ProjectionTest, ForwardIsFast) {
  std::string err;
  auto p = Projection::Create("EPSG:4258", "EPSG:2056", err);
  ASSERT_TRUE(p) << err;
  double out[3];
  constexpr int kN = 100000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kN; ++i) p->Forward(kLat + i * 1e-7, kLon, 2000.0, out);
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kN;
  EXPECT_LT(us, 10.0) << us << " us per point";
}
#endif

}  // namespace
}  // namespace lgrtk
