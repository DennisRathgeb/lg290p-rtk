// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/projection.h"

#include <chrono>
#include <string>

#include <gtest/gtest.h>

namespace lgrtk {
namespace {

#ifdef LGRTK_HAVE_PROJ
// Test point at Lai da Nalps (research note ntrip-and-swiss-coordinates.md,
// reference values from swisstopo REFRAME): ETRS89 46.63 N, 8.78 E.
constexpr double kLat = 46.63;
constexpr double kLon = 8.78;

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

TEST(FixProjectorTest, PicksByFrameAndRefusesUndeclared) {
  std::string err;
  auto p = FixProjector::Create(
      {.corrected_crs = "EPSG:4937", .autonomous_crs = ""}, "EPSG:2056", err);
  ASSERT_TRUE(p) << err;
  Fix f{};
  f.has_position = true;
  f.lat_deg = kLat;
  f.lon_deg = kLon;
  f.h_ell_m = 2000.0;
  f.frame = FixFrame::kCorrected;
  double out[3];
  EXPECT_TRUE(p->Project(f, out, err)) << err;
  EXPECT_NEAR(out[0], 2702718.1, 0.2);
  f.frame = FixFrame::kAutonomous;  // no CRS declared for autonomous fixes
  EXPECT_FALSE(p->Project(f, out, err));
  EXPECT_NE(err.find("autonomous"), std::string::npos) << err;
  f.has_position = false;
  EXPECT_FALSE(p->Project(f, out, err));
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
  for (int i = 0; i < kN; ++i) {
    p->Forward(kLat + i * 1e-7, kLon, 2000.0, out);
  }
  const double us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - t0)
                        .count() /
                    kN;
  EXPECT_LT(us, 10.0) << us << " us per point";
}
#endif

}  // namespace
}  // namespace lgrtk
