// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/lg290p.h"

#include <gtest/gtest.h>

#include "lgrtk/nmea.h"
#include "test_util.h"

namespace lgrtk::lg290p {
namespace {

TEST(Lg290pTest, BuildCommandChecksum) {
  EXPECT_EQ(BuildCommand("PQTMVERNO"), "$PQTMVERNO*58\r\n");
  EXPECT_EQ(BuildCommand("PQTMSAVEPAR"), "$PQTMSAVEPAR*5A\r\n");
  EXPECT_EQ(BuildCommand("PQTMSRR"), "$PQTMSRR*4B\r\n");
}

// Every example below is from the Quectel protocol specification v1.1.
TEST(Lg290pTest, CommandsMatchSpecExamples) {
  EXPECT_EQ(SetFixRate(1000)->text, "$PQTMCFGFIXRATE,W,1000*59\r\n");
  EXPECT_EQ(GetFixRate().text, "$PQTMCFGFIXRATE,R*71\r\n");
  EXPECT_EQ(SetMessageRate("GGA", 1).text, "$PQTMCFGMSGRATE,W,GGA,1*0A\r\n");
  EXPECT_EQ(SetMessageRate("PQTMEPE", 1, 2).text,
            "$PQTMCFGMSGRATE,W,PQTMEPE,1,2*1D\r\n");
  EXPECT_EQ(GetMessageRate("GGA").text, "$PQTMCFGMSGRATE,R,GGA*12\r\n");
  EXPECT_EQ(GetMessageRate("PQTMEPE", 2).text,
            "$PQTMCFGMSGRATE,R,PQTMEPE,2*05\r\n");
  EXPECT_EQ(SetNmeaDecimals({3, 8, 3, 2, 3, 2}).text,
            "$PQTMCFGNMEADP,W,3,8,3,2,3,2*39\r\n");
  EXPECT_EQ(SetRtk({1, 1, 60})->text, "$PQTMCFGRTK,W,1,1,60*46\r\n");
  EXPECT_EQ(SetConstellations({true, true, true, true, false, false}).text,
            "$PQTMCFGCNST,W,1,1,1,1,0,0*2B\r\n");
  EXPECT_EQ(SetElevationMask(5.0)->text, "$PQTMCFGELETHD,W,5.0*32\r\n");
  EXPECT_EQ(SetCnrMask(10.0)->text, "$PQTMCFGCNRTHD,W,10.0*15\r\n");
  EXPECT_EQ(SetReceiverMode(Mode::kBase).text, "$PQTMCFGRCVRMODE,W,2*29\r\n");
  EXPECT_EQ(SurveyIn(3600, 1.2)->text,
            "$PQTMCFGSVIN,W,1,3600,1.2,0,0,0*0A\r\n");
  EXPECT_EQ(
      FixedBase(-2519265.0514, 4849534.9045, 3277834.6432).text,
      "$PQTMCFGSVIN,W,2,0,0,-2519265.0514,4849534.9045,3277834.6432*2A\r\n");
  EXPECT_EQ(SetPps({true, 100, 1, 1})->text,
            "$PQTMCFGPPS,W,1,1,100,1,1,0*73\r\n");
  EXPECT_EQ(SetUartBaud(1, 115200)->text, "$PQTMCFGUART,W,1,115200*05\r\n");
  EXPECT_EQ(Restart(Start::kCold).text, "$PQTMCOLD*1C\r\n");
  EXPECT_EQ(Restart(Start::kWarm).text, "$PQTMWARM*11\r\n");
  EXPECT_EQ(Restart(Start::kHot).text, "$PQTMHOT*4B\r\n");
  EXPECT_EQ(QueryUniqueId().text, "$PQTMUNIQID*16\r\n");
  EXPECT_EQ(RestoreDefaults().text, "$PQTMRESTOREPAR*13\r\n");
}

TEST(Lg290pTest, ReplyExpectations) {
  EXPECT_EQ(GetFixRate().reply_to, "PQTMCFGFIXRATE");
  EXPECT_EQ(QueryVersion().reply_to, "PQTMVERNO");
  EXPECT_TRUE(SoftwareReset().reply_to.empty());  // the receiver just reboots
  EXPECT_TRUE(Restart(Start::kHot).reply_to.empty());
  EXPECT_TRUE(SetFixRate(100)->needs_reset);
  EXPECT_FALSE(SetMessageRate("GST", 1).needs_reset);
}

TEST(Lg290pTest, RestartByName) {
  EXPECT_EQ(RestartByName("hot")->text, Restart(Start::kHot).text);
  EXPECT_EQ(RestartByName("warm")->text, Restart(Start::kWarm).text);
  EXPECT_EQ(RestartByName("cold")->text, Restart(Start::kCold).text);
  EXPECT_EQ(RestartByName("soft")->text, SoftwareReset().text);
  EXPECT_FALSE(RestartByName("factory"));
  EXPECT_FALSE(RestartByName("lukewarm"));
}

TEST(Lg290pTest, RangesRejected) {
  EXPECT_FALSE(SetFixRate(49));
  EXPECT_FALSE(SetFixRate(1001));
  EXPECT_FALSE(SetRtk({1, 1, 0}));
  EXPECT_FALSE(SetRtk({3, 1, 60}));
  EXPECT_FALSE(SetElevationMask(91.0));
  EXPECT_FALSE(SetUartBaud(1, 12345));
  EXPECT_FALSE(SurveyIn(0, 1.0));
}

TEST(Lg290pTest, ReplyMatches) {
  std::array<std::string_view, nmea::kMaxFields> f{};
  const auto msg = nmea::ParseReply("$PQTMCFGMSGRATE,OK,PQTMEPE,1,2*4E", f);
  ASSERT_TRUE(msg);
  EXPECT_TRUE(Matches(GetMessageRate("PQTMEPE", 2), *msg));
  EXPECT_FALSE(Matches(GetMessageRate("GGA"), *msg));
  const auto fix = nmea::ParseReply("$PQTMCFGFIXRATE,OK,1000*0A", f);
  ASSERT_TRUE(fix);
  EXPECT_TRUE(Matches(GetFixRate(), *fix));
  EXPECT_FALSE(Matches(GetMessageRate("GGA"), *fix));
}

TEST(Lg290pTest, ReadBackFillsHave) {
  ReceiverConfig have;
  std::array<std::string_view, nmea::kMaxFields> f{};
  ApplyReadReply(nmea::Own(*nmea::ParseReply("$PQTMCFGFIXRATE,OK,1000*0A", f)),
                 have);
  ApplyReadReply(nmea::Own(*nmea::ParseReply("$PQTMCFGRTK,OK,1,1,120*20", f)),
                 have);
  ApplyReadReply(nmea::Own(*nmea::ParseReply("$PQTMCFGMSGRATE,OK,GGA,1*59", f)),
                 have);
  ApplyReadReply(
      nmea::Own(*nmea::ParseReply("$PQTMCFGMSGRATE,OK,PQTMEPE,1,2*4E", f)),
      have);
  ApplyReadReply(
      nmea::Own(*nmea::ParseReply("$PQTMCFGCNST,OK,1,1,1,1,0,0*78", f)), have);
  ApplyReadReply(nmea::Own(*nmea::ParseReply("$PQTMCFGELETHD,OK,5.0*61", f)),
                 have);
  ApplyReadReply(
      nmea::Own(*nmea::ParseReply("$PQTMCFGNMEADP,OK,3,8,3,2,3,2*6A", f)),
      have);
  EXPECT_EQ(have.fix_interval_ms, 1000);
  ASSERT_TRUE(have.rtk);
  EXPECT_EQ(have.rtk->timeout_s, 120);
  ASSERT_EQ(have.message_rates.size(), 2u);
  EXPECT_EQ(have.message_rates[1].name, "PQTMEPE");
  EXPECT_EQ(have.message_rates[1].version, 2);
  ASSERT_TRUE(have.constellations);
  EXPECT_FALSE(have.constellations->qzss);
  EXPECT_DOUBLE_EQ(*have.elevation_mask_deg, 5.0);
  ASSERT_TRUE(have.nmea_decimals);
  EXPECT_EQ(have.nmea_decimals->pos, 8);
}

TEST(Lg290pTest, ReadCommandsCoverWantedKeysOnly) {
  ReceiverConfig want;
  want.fix_interval_ms = 100;
  want.message_rates.push_back({"GST", 1, 0});
  const auto cmds = ReadCommands(want);
  ASSERT_EQ(cmds.size(), 2u);
  EXPECT_EQ(cmds[0].text, GetFixRate().text);
  EXPECT_EQ(cmds[1].text, GetMessageRate("GST").text);
}

TEST(Lg290pTest, PlanConfigEmptyWhenEqual) {
  ReceiverConfig want;
  want.fix_interval_ms = 100;
  want.message_rates.push_back({"GST", 1, 0});
  want.elevation_mask_deg = 10.0;
  ReceiverConfig have = want;
  EXPECT_TRUE(PlanConfig(want, have).empty());
}

TEST(Lg290pTest, PlanConfigAddsSaveAndResetOnlyWhenNeeded) {
  ReceiverConfig want;
  want.message_rates.push_back({"GST", 1, 0});
  ReceiverConfig have;
  have.message_rates.push_back({"GST", 0, 0});
  auto plan = PlanConfig(want, have);
  ASSERT_EQ(plan.size(), 2u);  // write + save, no reset
  EXPECT_EQ(plan[0].text, SetMessageRate("GST", 1).text);
  EXPECT_EQ(plan[1].text, SaveParams().text);

  want.fix_interval_ms = 50;
  have.fix_interval_ms = 100;
  plan = PlanConfig(want, have);
  ASSERT_EQ(plan.size(), 4u);  // two writes + save + reset
  EXPECT_EQ(plan[2].text, SaveParams().text);
  EXPECT_EQ(plan[3].text, SoftwareReset().text);
}

TEST(Lg290pTest, PlanConfigSkipsUnknownCurrentValues) {
  ReceiverConfig want;
  want.elevation_mask_deg = 10.0;
  want.message_rates.push_back({"GST", 1, 0});
  ReceiverConfig have;  // read-back failed: values unknown
  std::vector<std::string> unknown;
  EXPECT_TRUE(PlanConfig(want, have, &unknown)
                  .empty());  // no blind writes, no NVM wear
  ASSERT_EQ(unknown.size(), 2u);
}

TEST(Lg290pTest, RtkReplyWithoutTimeout) {
  ReceiverConfig have;
  std::array<std::string_view, nmea::kMaxFields> f{};
  ApplyReadReply(
      nmea::Own(*nmea::ParseReply(test::Nmea("PQTMCFGRTK,OK,1,1"), f)), have);
  ASSERT_TRUE(have.rtk);
  EXPECT_EQ(have.rtk->timeout_s, 0);  // unknown
  ReceiverConfig want;
  want.rtk = RtkSettings{1, 1, 30};
  EXPECT_TRUE(PlanConfig(want, have)
                  .empty());  // modes equal, timeout unknown: leave it
}

}  // namespace
}  // namespace lgrtk::lg290p
