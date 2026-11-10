// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/sky.h"

#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "test_util.h"

namespace lgrtk::daemon {
namespace {

using test::Nmea;

void FeedGpsGroup(SkyTracker* s) {
  s->FeedNmea(Nmea(
      "GPGSV,2,1,05,10,77,300,36,12,40,082,31,23,58,153,35,25,46,137,33,1"));
  s->FeedNmea(Nmea("GPGSV,2,2,05,32,10,020,,1"));
}

const SkySat* Find(const std::vector<SkySat>& v, int gnssid, int prn) {
  for (const auto& s : v) {
    if (s.gnssid == gnssid && s.prn == prn) {
      return &s;
    }
  }
  return nullptr;
}

TEST(SkyTest, GroupCompletesOnLastMessage) {
  SkyTracker s;
  FeedGpsGroup(&s);
  ASSERT_EQ(s.Satellites().size(), 5U);
  // A new group that is only half received keeps the previous list.
  s.FeedNmea(Nmea(
      "GPGSV,2,1,05,10,70,300,40,12,40,082,31,23,58,153,35,25,46,137,33,1"));
  auto sats = s.Satellites();
  ASSERT_EQ(sats.size(), 5U);
  EXPECT_EQ(Find(sats, 0, 10)->elevation_deg, 77);
  s.FeedNmea(Nmea("GPGSV,2,2,05,32,10,020,,1"));
  EXPECT_EQ(Find(s.Satellites(), 0, 10)->elevation_deg, 70);
}

TEST(SkyTest, UntrackedHasNoSignal) {
  SkyTracker s;
  FeedGpsGroup(&s);
  EXPECT_EQ(Find(s.Satellites(), 0, 32)->cn0_dbhz, -1);
}

TEST(SkyTest, UsedFromGsa) {
  SkyTracker s;
  FeedGpsGroup(&s);
  s.FeedNmea(Nmea("GNGSA,A,3,10,12,23,,,,,,,,,,2.38,1.26,2.01,1"));
  auto sats = s.Satellites();
  EXPECT_TRUE(Find(sats, 0, 10)->used);
  EXPECT_TRUE(Find(sats, 0, 23)->used);
  EXPECT_FALSE(Find(sats, 0, 25)->used);
  EXPECT_FLOAT_EQ(s.Hdop(), 1.26F);
  EXPECT_FLOAT_EQ(s.Pdop(), 2.38F);
}

TEST(SkyTest, TalkersKeptSeparately) {
  SkyTracker s;
  FeedGpsGroup(&s);
  s.FeedNmea(Nmea("GLGSV,1,1,02,65,30,100,40,66,20,200,38,1"));
  s.FeedNmea(Nmea("GNGSA,A,3,65,,,,,,,,,,,,2.0,1.0,1.7,2"));
  auto sats = s.Satellites();
  EXPECT_EQ(sats.size(), 7U);
  ASSERT_NE(Find(sats, 6, 65), nullptr);
  EXPECT_TRUE(Find(sats, 6, 65)->used);
  EXPECT_FALSE(Find(sats, 0, 10)->used);  // GPS used-set untouched by GLONASS
}

TEST(SkyTest, SignalsOfOneSatelliteMerge) {
  SkyTracker s;
  s.FeedNmea(Nmea("GAGSV,1,1,01,05,45,090,30,7"));  // E1
  s.FeedNmea(Nmea("GAGSV,1,1,01,05,45,090,42,1"));  // E5a
  auto sats = s.Satellites();
  ASSERT_EQ(sats.size(), 1U);
  EXPECT_EQ(sats[0].gnssid, 2);
  EXPECT_EQ(sats[0].cn0_dbhz, 42);  // strongest signal
}

TEST(SkyTest, BadChecksumIgnored) {
  SkyTracker s;
  s.FeedNmea("$GPGSV,1,1,01,10,77,300,36,1*00");
  EXPECT_TRUE(s.Satellites().empty());
}

TEST(SkyTest, SkyJsonCounts) {
  SkyTracker s;
  FeedGpsGroup(&s);
  s.FeedNmea(Nmea("GNGSA,A,3,10,12,,,,,,,,,,,2.38,1.26,2.01,1"));
  std::string line = SkyJson(s, "/dev/x", 1'789'084'800'000'000'000LL);
  ASSERT_EQ(line.substr(line.size() - 2), "\r\n");
  auto j = nlohmann::json::parse(line.substr(0, line.size() - 2));
  EXPECT_EQ(j["class"], "SKY");
  EXPECT_EQ(j["device"], "/dev/x");
  EXPECT_EQ(j["nSat"], 5);
  EXPECT_EQ(j["uSat"], 2);
  EXPECT_NEAR(j["hdop"].get<double>(), 1.26, 1e-6);
  ASSERT_EQ(j["satellites"].size(), 5U);
  EXPECT_EQ(j["satellites"][0]["PRN"], 10);
  EXPECT_EQ(j["satellites"][0]["gnssid"], 0);
  EXPECT_EQ(j["satellites"][0]["used"], true);
  EXPECT_TRUE(j.contains("time"));
  auto no_time = nlohmann::json::parse(SkyJson(s, "/dev/x", std::nullopt));
  EXPECT_FALSE(no_time.contains("time"));
}

}  // namespace
}  // namespace lgrtk::daemon
