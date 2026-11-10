// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/gpsd_json.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "lgrtk/version.h"

namespace lgrtk::daemon {
namespace {

using nlohmann::json;

Fix RtkFix() {
  Fix f{};
  f.utc_s = 43200.1;
  f.date_ymd = 20261110;
  f.lat_deg = 47.3769;
  f.lon_deg = 8.5417;
  f.h_ell_m = 500.0;
  f.geoid_sep_m = 48.0;
  f.sigma_e_m = 0.01F;
  f.sigma_n_m = 0.02F;
  f.sigma_u_m = 0.03F;
  f.sigma_source = SigmaSource::kGst;
  f.diff_age_s = 1.5F;
  f.station_id = 42;
  f.sats = 30;
  f.type = FixType::kRtkFixed;
  f.frame = FixFrame::kCorrected;
  f.has_position = true;
  f.vel_e_mps = 3.0F;
  f.vel_n_mps = 4.0F;
  f.vel_valid = true;
  return f;
}

json Parse(const std::string& s) {
  EXPECT_GE(s.size(), 2U);
  EXPECT_EQ(s.substr(s.size() - 2), "\r\n");
  return json::parse(s.substr(0, s.size() - 2));
}

TEST(GpsdJsonTest, TpvRtkFixed) {
  json j = Parse(TpvJson(RtkFix(), "/dev/lg290p0"));
  EXPECT_EQ(j["class"], "TPV");
  EXPECT_EQ(j["device"], "/dev/lg290p0");
  EXPECT_EQ(j["mode"], 3);
  EXPECT_EQ(j["status"], 3);
  EXPECT_DOUBLE_EQ(j["lat"].get<double>(), 47.3769);
  EXPECT_DOUBLE_EQ(j["lon"].get<double>(), 8.5417);
  EXPECT_DOUBLE_EQ(j["altHAE"].get<double>(), 500.0);
  EXPECT_DOUBLE_EQ(j["altMSL"].get<double>(), 452.0);
  EXPECT_DOUBLE_EQ(j["geoidSep"].get<double>(), 48.0);
  EXPECT_DOUBLE_EQ(j["dgpsAge"].get<double>(), 1.5);
  EXPECT_EQ(j["dgpsSta"], 42);
  EXPECT_EQ(j["time"], "2026-11-10T12:00:00.100Z");
  EXPECT_NEAR(j["speed"].get<double>(), 5.0, 1e-6);
  EXPECT_NEAR(j["track"].get<double>(), 36.8699, 1e-3);
}

TEST(GpsdJsonTest, TpvRtkFloatAndOthers) {
  Fix f = RtkFix();
  f.type = FixType::kRtkFloat;
  EXPECT_EQ(Parse(TpvJson(f, "d"))["status"], 4);
  f.type = FixType::kDgnss;
  EXPECT_EQ(Parse(TpvJson(f, "d"))["status"], 2);
  f.type = FixType::kSingle;
  f.diff_age_s = 0;
  json j = Parse(TpvJson(f, "d"));
  EXPECT_EQ(j["status"], 1);
  EXPECT_FALSE(j.contains("dgpsAge"));
  EXPECT_FALSE(j.contains("dgpsSta"));
}

TEST(GpsdJsonTest, NoPositionIsMode1WithoutCoordinates) {
  Fix f = RtkFix();
  f.has_position = false;
  f.type = FixType::kNone;
  json j = Parse(TpvJson(f, "d"));
  EXPECT_EQ(j["mode"], 1);
  EXPECT_FALSE(j.contains("lat"));
  EXPECT_FALSE(j.contains("epx"));
}

TEST(GpsdJsonTest, NoTimeWithoutDate) {
  Fix f = RtkFix();
  f.date_ymd = 0;
  EXPECT_FALSE(FixUnixNs(f));
  EXPECT_FALSE(Parse(TpvJson(f, "d")).contains("time"));
}

TEST(GpsdJsonTest, IsoTimeMillis) {
  auto ns = FixUnixNs(RtkFix());
  ASSERT_TRUE(ns);
  EXPECT_EQ(IsoTime(*ns), "2026-11-10T12:00:00.100Z");
  EXPECT_EQ(IsoTime(0), "1970-01-01T00:00:00.000Z");
  Fix f = RtkFix();
  f.utc_s = 86399.999;
  EXPECT_EQ(IsoTime(*FixUnixNs(f)), "2026-11-10T23:59:59.999Z");
  f.date_ymd = 20240229;
  f.utc_s = 0;
  EXPECT_EQ(*FixUnixNs(f), 1709164800LL * 1000000000LL);
}

TEST(GpsdJsonTest, EpFromSigmas) {
  json j = Parse(TpvJson(RtkFix(), "d"));
  EXPECT_NEAR(j["epx"].get<double>(), 1.96 * 0.01, 1e-6);
  EXPECT_NEAR(j["epy"].get<double>(), 1.96 * 0.02, 1e-6);
  EXPECT_NEAR(j["epv"].get<double>(), 1.96 * 0.03, 1e-6);
  Fix f = RtkFix();
  f.sigma_source = SigmaSource::kNone;
  EXPECT_FALSE(Parse(TpvJson(f, "d")).contains("epx"));
}

TEST(GpsdJsonTest, Gst) {
  json j = Parse(GstJson(RtkFix(), "d"));
  EXPECT_EQ(j["class"], "GST");
  EXPECT_NEAR(j["lat"].get<double>(), 0.02, 1e-6);
  EXPECT_NEAR(j["lon"].get<double>(), 0.01, 1e-6);
  EXPECT_NEAR(j["alt"].get<double>(), 0.03, 1e-6);
  EXPECT_EQ(j["time"], "2026-11-10T12:00:00.100Z");
}

TEST(GpsdJsonTest, GstOmittedWithoutSigma) {
  Fix f = RtkFix();
  f.sigma_source = SigmaSource::kNone;
  EXPECT_EQ(GstJson(f, "d"), "");
}

TEST(GpsdJsonTest, VersionDevicesWatchNoFix) {
  json v = Parse(VersionJson());
  EXPECT_EQ(v["class"], "VERSION");
  EXPECT_EQ(v["release"], std::string("lgrtkd ") + kVersion);
  EXPECT_EQ(v["proto_major"], 3);
  json d = Parse(DevicesJson("/dev/x", true, 460800));
  EXPECT_EQ(d["class"], "DEVICES");
  ASSERT_EQ(d["devices"].size(), 1U);
  EXPECT_EQ(d["devices"][0]["path"], "/dev/x");
  EXPECT_EQ(d["devices"][0]["bps"], 460800);
  EXPECT_EQ(d["devices"][0]["driver"], "Quectel LG290P");
  json off = Parse(DevicesJson("/dev/x", false, 0));
  EXPECT_EQ(off["devices"][0]["activated"], 0);
  json w = Parse(WatchJson(Watch{true, true, false}));
  EXPECT_EQ(w["class"], "WATCH");
  EXPECT_EQ(w["enable"], true);
  EXPECT_EQ(w["json"], true);
  EXPECT_EQ(w["nmea"], false);
  json n = Parse(NoFixTpvJson("/dev/x"));
  EXPECT_EQ(n["mode"], 0);
  json e = Parse(ErrorJson("bad \"thing\""));
  EXPECT_EQ(e["class"], "ERROR");
  EXPECT_EQ(e["message"], "bad \"thing\"");
}

TEST(GpsdJsonTest, ParsesWatchEnable) {
  auto c = ParseGpsdCommand("?WATCH={\"enable\":true,\"json\":true};");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->kind, GpsdCommand::kWatch);
  EXPECT_EQ(c->arg["enable"], true);
  c = ParseGpsdCommand("?WATCH;");
  ASSERT_TRUE(c);
  EXPECT_EQ(c->kind, GpsdCommand::kWatch);
  EXPECT_TRUE(c->arg.is_null());
  c = ParseGpsdCommand("?WATCH={\"enable\":true}");  // no trailing ';'
  ASSERT_TRUE(c);
  EXPECT_EQ(c->kind, GpsdCommand::kWatch);
}

TEST(GpsdJsonTest, ParsesBareCommands) {
  EXPECT_EQ(ParseGpsdCommand("?POLL;")->kind, GpsdCommand::kPoll);
  EXPECT_EQ(ParseGpsdCommand("?VERSION;")->kind, GpsdCommand::kVersion);
  EXPECT_EQ(ParseGpsdCommand("?DEVICES;")->kind, GpsdCommand::kDevices);
  EXPECT_EQ(ParseGpsdCommand("?FOO;")->kind, GpsdCommand::kUnknown);
  EXPECT_EQ(ParseGpsdCommand("hello")->kind, GpsdCommand::kUnknown);
}

TEST(GpsdJsonTest, MalformedWatch) {
  EXPECT_FALSE(ParseGpsdCommand("?WATCH={\"enable\":tru};"));
  EXPECT_FALSE(ParseGpsdCommand("?WATCH=[1,2];"));  // must be an object
}

TEST(GpsdJsonTest, ApplyWatch) {
  Watch w;
  ApplyWatch(json::parse(R"({"enable":true,"json":true})"), &w);
  EXPECT_TRUE(w.enable);
  EXPECT_TRUE(w.json);
  EXPECT_FALSE(w.nmea);
  ApplyWatch(json::parse(R"({"nmea":true,"json":"yes"})"), &w);  // bad type
  EXPECT_TRUE(w.nmea);
  EXPECT_TRUE(w.json);
  ApplyWatch(json::parse(R"({"enable":false})"), &w);
  EXPECT_FALSE(w.enable);
}

TEST(GpsdJsonTest, EveryObjectEndsCrLf) {
  for (const std::string& s :
       {VersionJson(), DevicesJson("d", true, 1), TpvJson(RtkFix(), "d"),
        GstJson(RtkFix(), "d"), NoFixTpvJson("d"), WatchJson(Watch{}),
        ErrorJson("x")}) {
    ASSERT_GE(s.size(), 2U);
    EXPECT_EQ(s.substr(s.size() - 2), "\r\n");
    EXPECT_EQ(s.find('\n'), s.size() - 1) << "one line only: " << s;
  }
}

}  // namespace
}  // namespace lgrtk::daemon
