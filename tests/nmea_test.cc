// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/nmea.h"

#include <array>

#include <gtest/gtest.h>

#include "test_util.h"

namespace lgrtk::nmea {
namespace {

// Example lines from the Quectel LG290P(03) protocol specification v1.1.
constexpr const char* kGga =
    "$GNGGA,025159.000,3149.29993210,N,11706.91264104,E,1,16,1.26,97.250,M,-4."
    "945,M,,*5A";
constexpr const char* kRmc =
    "$GNRMC,025159.000,A,3149.29993210,N,11706.91264104,E,0.001,043.43,291123,,"
    ",A,V*33";
constexpr const char* kGst =
    "$GNGST,013902.500,5.387,3.734,2.172,143.088,2.836,3.258,9.274*74";
constexpr const char* kGsv =
    "$GPGSV,2,1,05,10,77,300,36,12,40,082,31,23,58,153,35,25,46,137,33,1*67";
constexpr const char* kGsa =
    "$GNGSA,A,3,10,12,23,25,32,,,,,,,,2.38,1.26,2.01,1*0B";
constexpr const char* kVtg = "$GNVTG,043.43,T,,M,0.001,N,0.001,K,A*23";
constexpr const char* kZda = "$GNZDA,102210.014,23,12,2021,00,00*4E";
constexpr const char* kEpe = "$PQTMEPE,2,1.000,1.000,1.000,1.414,1.732*52";
constexpr const char* kVerno =
    "$PQTMVERNO,LG290P03AANR01A03S,2024/04/30,10:53:07*18";
constexpr const char* kSvin =
    "$PQTMSVINSTATUS,1,1000,1,,01,20,100,-2484434.3645,4875976.9741,3266161."
    "3412,1.2415*3C";

TEST(NmeaTest, Gga) {
  const auto g = ParseGga(kGga);
  ASSERT_TRUE(g);
  EXPECT_DOUBLE_EQ(g->utc_s, 2 * 3600 + 51 * 60 + 59.0);
  EXPECT_NEAR(g->lat_deg, 31 + 49.29993210 / 60, 1e-12);
  EXPECT_NEAR(g->lon_deg, 117 + 6.91264104 / 60, 1e-12);
  EXPECT_EQ(g->quality, 1);
  EXPECT_EQ(g->sats, 16);
  EXPECT_DOUBLE_EQ(g->hdop, 1.26);
  EXPECT_DOUBLE_EQ(g->alt_msl_m, 97.25);
  EXPECT_DOUBLE_EQ(g->geoid_sep_m, -4.945);
  EXPECT_FALSE(g->has_diff_age);
}

TEST(NmeaTest, GgaEmptyFieldsFromCapture) {
  const auto g = ParseGga("$GNGGA,003508.100,,,,,0,00,99.99,,M,,M,,*47");
  ASSERT_TRUE(g);
  EXPECT_EQ(g->quality, 0);
  EXPECT_FALSE(g->has_position);
  EXPECT_DOUBLE_EQ(g->hdop, 99.99);
}

TEST(NmeaTest, GgaWithDiffAge) {
  const auto g =
      ParseGga(test::Nmea("GNGGA,120000.000,4637.80000000,N,00846.80000000,E,4,"
                          "30,0.5,1950.0,M,49.1,M,1.2,0123"));
  ASSERT_TRUE(g);
  EXPECT_EQ(g->quality, 4);
  EXPECT_TRUE(g->has_diff_age);
  EXPECT_DOUBLE_EQ(g->diff_age_s, 1.2);
  EXPECT_EQ(g->station_id, 123);
}

TEST(NmeaTest, WrongTypeAndBadChecksumRejected) {
  EXPECT_FALSE(ParseGga(kRmc));
  EXPECT_FALSE(
      ParseGga("$GNGGA,025159.000,3149.29993210,N,11706.91264104,E,1,16,1.26,"
               "97.250,M,-4.945,M,,*00"));
}

TEST(NmeaTest, Rmc) {
  const auto r = ParseRmc(kRmc);
  ASSERT_TRUE(r);
  EXPECT_TRUE(r->valid);
  EXPECT_EQ(r->date_ymd, 20231129);
  EXPECT_NEAR(r->speed_mps, 0.001 * 1852.0 / 3600.0, 1e-9);
  EXPECT_DOUBLE_EQ(r->course_deg, 43.43);
}

TEST(NmeaTest, RmcReceiverDefaultDateIsUnknown) {
  // Before the receiver knows the time it reports 1980-01-06 (GPS epoch).
  const auto r = ParseRmc("$GNRMC,003508.100,V,,,,,,,060180,,,N,V*29");
  ASSERT_TRUE(r);
  EXPECT_FALSE(r->valid);
  EXPECT_EQ(r->date_ymd, 0);
}

TEST(NmeaTest, Gst) {
  const auto g = ParseGst(kGst);
  ASSERT_TRUE(g);
  EXPECT_TRUE(g->valid);
  EXPECT_DOUBLE_EQ(g->sigma_lat_m, 2.836);
  EXPECT_DOUBLE_EQ(g->sigma_lon_m, 3.258);
  EXPECT_DOUBLE_EQ(g->sigma_alt_m, 9.274);
  const auto empty = ParseGst(test::Nmea("GNGST,000001.000,,,,,,,"));
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty->valid);
}

TEST(NmeaTest, Gsv) {
  const auto g = ParseGsv(kGsv);
  ASSERT_TRUE(g);
  EXPECT_EQ(g->talker, Talker::kGps);
  EXPECT_EQ(g->total_msgs, 2);
  EXPECT_EQ(g->msg_num, 1);
  EXPECT_EQ(g->in_view, 5);
  EXPECT_EQ(g->signal_id, 1);
  ASSERT_EQ(g->count, 4);
  EXPECT_EQ(g->sats[0].prn, 10);
  EXPECT_EQ(g->sats[0].elev_deg, 77);
  EXPECT_EQ(g->sats[0].az_deg, 300);
  EXPECT_EQ(g->sats[0].cn0_dbhz, 36);
  const auto empty = ParseGsv("$GPGSV,1,1,00,1*64");
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty->count, 0);
}

TEST(NmeaTest, Gsa) {
  const auto g = ParseGsa(kGsa);
  ASSERT_TRUE(g);
  EXPECT_EQ(g->fix_mode, 3);
  EXPECT_DOUBLE_EQ(g->pdop, 2.38);
  EXPECT_DOUBLE_EQ(g->hdop, 1.26);
  EXPECT_DOUBLE_EQ(g->vdop, 2.01);
  EXPECT_EQ(g->system_id, 1);
  EXPECT_EQ(g->used_count, 5);
}

TEST(NmeaTest, VtgAndZda) {
  const auto v = ParseVtg(kVtg);
  ASSERT_TRUE(v);
  EXPECT_DOUBLE_EQ(v->course_deg, 43.43);
  EXPECT_NEAR(v->speed_mps, 0.001 / 3.6, 1e-12);
  const auto z = ParseZda(kZda);
  ASSERT_TRUE(z);
  EXPECT_EQ(z->date_ymd, 20211223);
  EXPECT_NEAR(z->utc_s, 10 * 3600 + 22 * 60 + 10.014, 1e-9);
}

TEST(NmeaTest, PqtmEpe) {
  const auto e = ParseEpe(kEpe);
  ASSERT_TRUE(e);
  EXPECT_DOUBLE_EQ(e->north_m, 1.0);
  EXPECT_DOUBLE_EQ(e->h2d_m, 1.414);
  EXPECT_DOUBLE_EQ(e->h3d_m, 1.732);
}

TEST(NmeaTest, PqtmVerno) {
  const auto v = ParseVerno(kVerno);
  ASSERT_TRUE(v);
  EXPECT_EQ(v->version, "LG290P03AANR01A03S");
  EXPECT_EQ(v->build_date, "2024/04/30");
  EXPECT_EQ(v->build_time, "10:53:07");
}

TEST(NmeaTest, PqtmSvinStatus) {
  const auto s = ParseSvinStatus(kSvin);
  ASSERT_TRUE(s);
  EXPECT_EQ(s->tow_ms, 1000u);
  EXPECT_EQ(s->valid, 1);
  EXPECT_EQ(s->observations, 20u);
  EXPECT_EQ(s->configured_duration, 100u);
  EXPECT_DOUBLE_EQ(s->mean_x_m, -2484434.3645);
  EXPECT_DOUBLE_EQ(s->mean_acc_m, 1.2415);
}

TEST(NmeaTest, ReplyOkWithValues) {
  std::array<std::string_view, kMaxFields> fields{};
  const auto r = ParseReply("$PQTMCFGMSGRATE,OK,PQTMEPE,1,2*4E", fields);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->command, "PQTMCFGMSGRATE");
  EXPECT_TRUE(r->ok);
  ASSERT_EQ(r->values.size(), 3u);
  EXPECT_EQ(r->values[0], "PQTMEPE");
  EXPECT_EQ(r->values[2], "2");
  const OwnedReply o = Own(*r);
  EXPECT_EQ(o.values.at(1), "1");
}

TEST(NmeaTest, ReplyOkBareAndError) {
  std::array<std::string_view, kMaxFields> fields{};
  const auto ok = ParseReply("$PQTMSAVEPAR,OK*72", fields);
  ASSERT_TRUE(ok);
  EXPECT_TRUE(ok->ok);
  EXPECT_TRUE(ok->values.empty());
  const auto err = ParseReply(test::Nmea("PQTMCFGMSGRATE,ERROR,1"), fields);
  ASSERT_TRUE(err);
  EXPECT_FALSE(err->ok);
  EXPECT_EQ(err->error_code, 1);
  // A PQTM output message (not a reply) is not a reply.
  EXPECT_FALSE(ParseReply(kEpe, fields));
  // VERNO answers without OK: treated as a reply carrying the values.
  const auto ver = ParseReply(kVerno, fields);
  ASSERT_TRUE(ver);
  EXPECT_TRUE(ver->ok);
  EXPECT_EQ(ver->values[0], "LG290P03AANR01A03S");
}

TEST(NmeaTest, SentenceTypeIsTalkerAgnostic) {
  EXPECT_EQ(SentenceType("GNGGA,1"), "GGA");
  EXPECT_EQ(SentenceType("GPGSV,1"), "GSV");
  EXPECT_EQ(SentenceType("PQTMEPE,2"), "PQTMEPE");
  EXPECT_EQ(TalkerOf("GLGSV,1"), Talker::kGlonass);
  EXPECT_EQ(TalkerOf("GBGSV,1"), Talker::kBeidou);
}

}  // namespace
}  // namespace lgrtk::nmea
