// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/epoch.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "test_util.h"

namespace lgrtk {
namespace {

using test::Nmea;

std::string Gga(const char* utc, int q = 1, const char* age = "") {
  return Nmea(std::string("GNGGA,") + utc +
              ",4637.80000000,N,00846.80000000,E," + std::to_string(q) +
              ",20,0.80,1950.000,M,49.100,M," + age + "," +
              (age[0] ? "0042" : ""));
}
std::string Rmc(const char* utc) {
  return Nmea(std::string("GNRMC,") + utc +
              ",A,4637.80000000,N,00846.80000000,E,1.944,090.00,101126,,,A,V");
}
std::string Gst(const char* utc) {
  return Nmea(std::string("GNGST,") + utc +
              ",0.5,0.03,0.02,10.0,0.020,0.010,0.040");
}

struct Feeder {
  EpochAssembler a;
  std::vector<Fix> fixes;
  int64_t t = 0;
  bool Feed(const std::string& line) {
    Fix f{};
    t += 1000;
    if (a.Feed(line, t, f)) {
      fixes.push_back(f);
      return true;
    }
    return false;
  }
};

TEST(EpochTest, GgaAloneYieldsFixWithoutSigma) {
  Feeder r;
  EXPECT_FALSE(r.Feed(Gga("100000.000")));
  EXPECT_TRUE(r.Feed(Gga("100000.100")));  // next epoch closes the first
  ASSERT_EQ(r.fixes.size(), 1u);
  const Fix& f = r.fixes[0];
  EXPECT_EQ(f.type, FixType::kSingle);
  EXPECT_EQ(f.sigma_source, SigmaSource::kNone);
  EXPECT_TRUE(f.has_position);
  EXPECT_NEAR(f.lat_deg, 46 + 37.8 / 60, 1e-12);
  EXPECT_NEAR(f.lon_deg, 8 + 46.8 / 60, 1e-12);
  EXPECT_NEAR(f.h_ell_m, 1950.0 + 49.1, 1e-9);
  EXPECT_EQ(f.sats, 20);
  EXPECT_EQ(f.frame, FixFrame::kAutonomous);
  EXPECT_DOUBLE_EQ(f.utc_s, 36000.0);
  EXPECT_EQ(f.t_arrival_ns, 1000);  // arrival of the epoch's last sentence
}

TEST(EpochTest, GstBeforeOrAfterGgaPairs) {
  for (bool gst_first : {true, false}) {
    Feeder r;
    if (gst_first) {
      r.Feed(Gst("100000.000"));
      r.Feed(Gga("100000.000"));
    } else {
      r.Feed(Gga("100000.000"));
      r.Feed(Gst("100000.000"));
    }
    r.Feed(Gga("100000.100"));
    ASSERT_EQ(r.fixes.size(), 1u);
    EXPECT_EQ(r.fixes[0].sigma_source, SigmaSource::kGst);
    EXPECT_FLOAT_EQ(r.fixes[0].sigma_n_m, 0.020f);
    EXPECT_FLOAT_EQ(r.fixes[0].sigma_e_m, 0.010f);
    EXPECT_FLOAT_EQ(r.fixes[0].sigma_u_m, 0.040f);
  }
}

TEST(EpochTest, ClosesWithoutWaitingWhenComplete) {
  Feeder r;
  r.Feed(Gga("100000.000"));
  r.Feed(Rmc("100000.000"));
  r.Feed(Gst("100000.000"));
  EXPECT_TRUE(
      r.Feed(Gga("100000.100")));  // closes epoch 1 (pattern still unknown)
  EXPECT_FALSE(r.Feed(Rmc("100000.100")));
  EXPECT_TRUE(r.Feed(Gst("100000.100")));  // pattern learnt: complete now
  ASSERT_EQ(r.fixes.size(), 2u);
  EXPECT_DOUBLE_EQ(r.fixes[1].utc_s, 36000.1);
  EXPECT_EQ(r.fixes[1].t_arrival_ns, 6000);
  EXPECT_EQ(r.fixes[1].date_ymd, 20261110);
  EXPECT_TRUE(r.fixes[1].vel_valid);
  EXPECT_NEAR(r.fixes[1].vel_e_mps, 1.944 * 1852 / 3600, 1e-4);
  EXPECT_NEAR(r.fixes[1].vel_n_mps, 0.0, 1e-4);
  // A late duplicate sentence of an emitted epoch does not emit again.
  EXPECT_FALSE(r.Feed(Rmc("100000.100")));
}

TEST(EpochTest, CorrectedFrameNeedsDiffAge) {
  Feeder r;
  r.Feed(Gga("100000.000", 4, "1.2"));
  r.Feed(Gga("100000.100", 4, ""));
  r.Feed(Gga("100000.200"));
  ASSERT_EQ(r.fixes.size(), 2u);
  EXPECT_EQ(r.fixes[0].type, FixType::kRtkFixed);
  EXPECT_EQ(r.fixes[0].frame, FixFrame::kCorrected);
  EXPECT_FLOAT_EQ(r.fixes[0].diff_age_s, 1.2f);
  EXPECT_EQ(r.fixes[0].station_id, 42);
  EXPECT_EQ(r.fixes[1].frame, FixFrame::kAutonomous);
}

TEST(EpochTest, SbasDgnssIsAutonomous) {
  // Quality 2 from SBAS (station id = SBAS PRN 120..158) is in the broadcast
  // frame.
  Feeder r;
  r.Feed(
      Nmea("GNGGA,100000.000,4637.80000000,N,00846.80000000,E,2,20,0.80,1950."
           "000,M,49.100,M,1.0,0123"));
  r.Feed(Gga("100000.100"));
  ASSERT_EQ(r.fixes.size(), 1u);
  EXPECT_EQ(r.fixes[0].type, FixType::kDgnss);
  EXPECT_EQ(r.fixes[0].frame, FixFrame::kAutonomous);
}

TEST(EpochTest, EpeUsedWhenNoGst) {
  Feeder r;
  r.Feed(Gga("100000.000"));
  r.Feed(Nmea("PQTMEPE,2,0.300,0.400,0.900,0.500,1.030"));
  r.Feed(Gga("100000.100"));
  ASSERT_EQ(r.fixes.size(), 1u);
  EXPECT_EQ(r.fixes[0].sigma_source, SigmaSource::kEpe);
  EXPECT_FLOAT_EQ(r.fixes[0].sigma_n_m, 0.3f);
  EXPECT_FLOAT_EQ(r.fixes[0].sigma_e_m, 0.4f);
  EXPECT_FLOAT_EQ(r.fixes[0].sigma_u_m, 0.9f);
}

TEST(EpochTest, SeqIncrementsAndFlush) {
  Feeder r;
  r.Feed(Gga("100000.000"));
  r.Feed(Gga("100000.100"));
  Fix f{};
  ASSERT_TRUE(r.a.Flush(f));
  EXPECT_EQ(r.fixes[0].seq + 1, f.seq);
  EXPECT_FALSE(r.a.Flush(f));
}

TEST(EpochTest, IgnoresOtherSentences) {
  Feeder r;
  EXPECT_FALSE(r.Feed(Nmea("GPGSV,1,1,00,1")));
  EXPECT_FALSE(r.Feed("garbage"));
  EXPECT_TRUE(r.fixes.empty());
}

TEST(EpochTest, ReplayCaptureGivesTenHz) {
  std::ifstream in(LGRTK_TEST_DATA "/probe1.nmea");
  ASSERT_TRUE(in);
  Feeder r;
  std::string line;
  while (std::getline(in, line)) {
    r.Feed(line);
  }
  // 30 s at 10 Hz, indoors without a fix.
  EXPECT_GE(r.fixes.size(), 295u);
  EXPECT_LE(r.fixes.size(), 305u);
  for (const Fix& f : r.fixes) {
    EXPECT_EQ(f.type, FixType::kNone);
    EXPECT_FALSE(f.has_position);
  }
}

TEST(EpochTest, FixTypeMapping) {
  EXPECT_EQ(FixTypeFromGgaQuality(0), FixType::kNone);
  EXPECT_EQ(FixTypeFromGgaQuality(2), FixType::kDgnss);
  EXPECT_EQ(FixTypeFromGgaQuality(4), FixType::kRtkFixed);
  EXPECT_EQ(FixTypeFromGgaQuality(5), FixType::kRtkFloat);
  EXPECT_EQ(FixTypeFromGgaQuality(9), FixType::kNone);
  EXPECT_STREQ(ToString(FixType::kRtkFixed), "rtk-fixed");
}

TEST(EpochTest, GgaQualityRoundTrip) {
  for (uint8_t q = 0; q <= 8; ++q) {
    EXPECT_EQ(ToGgaQuality(FixTypeFromGgaQuality(q)), q);
  }
  EXPECT_EQ(ToGgaQuality(FixType::kRtkFloat), 5);
  EXPECT_EQ(ToGgaQuality(FixTypeFromGgaQuality(9)), 0);  // unknown -> kNone
}

}  // namespace
}  // namespace lgrtk
