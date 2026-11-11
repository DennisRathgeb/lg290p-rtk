// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/ntrip.h"

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "sim/fake_caster.h"
#include "test_util.h"

namespace lgrtk {
namespace {

using namespace std::chrono_literals;
using test::FakeCaster;
using test::WaitFor;

struct VecSink : CorrectionSink {
  std::mutex mu;
  std::vector<std::vector<uint8_t>> frames;
  bool Inject(std::span<const uint8_t> f) override {
    std::scoped_lock lk(mu);
    frames.emplace_back(f.begin(), f.end());
    return true;
  }
  size_t Count() {
    std::scoped_lock lk(mu);
    return frames.size();
  }
};

NtripOptions Opts(const FakeCaster& c, int version = 2) {
  NtripOptions o;
  o.host = "127.0.0.1";
  o.port = c.Port();
  o.mountpoint = "NALPS";
  o.user = "dennis";
  o.password = "s3cret-pw";
  o.version = version;
  o.send_gga = false;
  o.backoff_min_ms = 50;
  o.backoff_max_ms = 200;
  o.stall_after_ms = 300;
  return o;
}

Fix SomeFix() {
  Fix f{};
  f.has_position = true;
  f.lat_deg = 46.63;
  f.lon_deg = 8.78;
  f.h_ell_m = 2000.0;
  f.geoid_sep_m = 49.1;
  f.type = FixType::kSingle;
  f.sats = 20;
  f.hdop = 0.8f;
  f.utc_s = 36000.0;
  return f;
}

TEST(NtripTest, V2ChunkedStreamsToSink) {
  FakeCaster caster(FakeCaster::Mode::kV2Chunked);
  VecSink sink;
  NtripClient c(Opts(caster), sink);
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(WaitFor([&] { return sink.Count() >= 3; }, 2s));
  EXPECT_EQ(c.Status().state, NtripState::kStreaming);
  {
    std::scoped_lock lk(sink.mu);
    for (const auto& f : sink.frames) {
      EXPECT_EQ(f, FakeCaster::Frame());
    }
  }
  const auto req = caster.Requests().at(0);
  EXPECT_NE(req.find("GET /NALPS HTTP/1.1"), std::string::npos);
  EXPECT_NE(req.find("Ntrip-Version: Ntrip/2.0"), std::string::npos);
  EXPECT_NE(req.find("Authorization: Basic ZGVubmlzOnMzY3JldC1wdw=="),
            std::string::npos);
  EXPECT_NE(req.find("User-Agent: NTRIP lgrtk/"), std::string::npos);
}

TEST(NtripTest, V1IcyResponse) {
  FakeCaster caster(FakeCaster::Mode::kV1Icy);
  VecSink sink;
  NtripClient c(Opts(caster, 1), sink);
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(WaitFor([&] { return sink.Count() >= 3; }, 2s));
  EXPECT_NE(caster.Requests().at(0).find("GET /NALPS HTTP/1.0"),
            std::string::npos);
}

TEST(NtripTest, SendsGgaPeriodically) {
  FakeCaster caster(FakeCaster::Mode::kV2Chunked);
  VecSink sink;
  auto o = Opts(caster);
  o.send_gga = true;
  o.gga_interval_ms = 200;
  NtripClient c(o, sink);
  c.SetPosition(SomeFix());
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(WaitFor(
      [&] {
        const std::string b = caster.ClientBytes();
        size_t n = 0;
        for (size_t p = b.find("$GPGGA"); p != std::string::npos;
             p = b.find("$GPGGA", p + 1)) {
          ++n;
        }
        return n >= 3;
      },
      3s));
  const std::string b = caster.ClientBytes();
  EXPECT_NE(b.find("$GPGGA,100000.00,4637.8000000,N,00846.8000000,E,1,20,0.8,"
                   "1950.900,M,49.100,M,,*"),
            std::string::npos)
      << b;
  EXPECT_NE(caster.Requests().at(0).find("Ntrip-GGA: $GPGGA"),
            std::string::npos);
}

TEST(NtripTest, NoPositionWaitsBeforeConnecting) {
  FakeCaster caster(FakeCaster::Mode::kV2Chunked);
  VecSink sink;
  auto o = Opts(caster);
  o.send_gga = true;
  NtripClient c(o, sink);
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(
      WaitFor([&] { return c.Status().state == NtripState::kNoPosition; }, 1s));
  EXPECT_EQ(caster.Connections(), 0);
  c.SetPosition(SomeFix());
  ASSERT_TRUE(WaitFor([&] { return sink.Count() >= 1; }, 2s));
}

TEST(NtripTest, AuthFailureStops) {
  FakeCaster caster(FakeCaster::Mode::kUnauthorized);
  VecSink sink;
  NtripClient c(Opts(caster), sink);
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(
      WaitFor([&] { return c.Status().state == NtripState::kAuthFailed; }, 2s));
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(caster.Connections(), 1);  // no hammering
  EXPECT_EQ(c.Status().http_status, 401);
  // The error says what to check, not just the status.
  EXPECT_NE(c.Status().last_error.find("check user name and password"),
            std::string::npos)
      << c.Status().last_error;
}

// A proxy answers 200 at once and passes the backend's 401 on in the stream
// (IQProxy): still an authentication failure, one connection, no retries.
TEST(NtripTest, AuthFailureInStreamStopsV1) {
  FakeCaster caster(FakeCaster::Mode::kProxyUnauthorizedV1);
  VecSink sink;
  NtripOptions o = Opts(caster, 1);
  o.send_gga = true;
  NtripClient c(o, sink);
  c.SetPosition(SomeFix());
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(
      WaitFor([&] { return c.Status().state == NtripState::kAuthFailed; }, 3s))
      << c.Status().last_error;
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(caster.Connections(), 1);
  EXPECT_EQ(c.Status().http_status, 401);
  EXPECT_EQ(sink.Count(), 0u);
}

TEST(NtripTest, AuthFailureInStreamStopsV2) {
  FakeCaster caster(FakeCaster::Mode::kProxyUnauthorizedV2);
  VecSink sink;
  NtripClient c(Opts(caster), sink);
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(
      WaitFor([&] { return c.Status().state == NtripState::kAuthFailed; }, 3s))
      << c.Status().last_error;
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(caster.Connections(), 1);
  EXPECT_EQ(c.Status().http_status, 401);
}

// Another error in the stream is a failure with backoff: the bytes of the
// error are not corrections, so they must not reset the backoff.
TEST(NtripTest, ErrorInStreamBacksOff) {
  FakeCaster caster(FakeCaster::Mode::kProxyUnavailable);
  VecSink sink;
  NtripOptions o = Opts(caster);
  o.backoff_min_ms = 50;
  o.backoff_max_ms = 400;
  NtripClient c(o, sink);
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(WaitFor(
      [&] { return c.Status().last_error.find("503") != std::string::npos; },
      2s))
      << c.Status().last_error;
  std::this_thread::sleep_for(1s);
  c.Stop();
  // 50 + 100 + 200 + 400 ms: at most about five connections in the second,
  // not the twenty a reset backoff would make.
  EXPECT_LE(caster.Connections(), 6);
  EXPECT_EQ(sink.Count(), 0u);
}

TEST(NtripTest, ReconnectsAfterDrop) {
  FakeCaster caster(FakeCaster::Mode::kDropAfterOne);
  VecSink sink;
  NtripClient c(Opts(caster), sink);
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(WaitFor(
      [&] { return caster.Connections() >= 2 && sink.Count() >= 2; }, 3s));
  EXPECT_GE(c.Status().connects, 2u);
}

TEST(NtripTest, StalledWhenSilent) {
  FakeCaster caster(FakeCaster::Mode::kSilent);
  VecSink sink;
  std::mutex mu;
  std::vector<NtripState> seen;
  NtripClient c(Opts(caster), sink);
  c.OnStatus([&](NtripState, NtripState to, const std::string&) {
    std::scoped_lock lk(mu);
    seen.push_back(to);
  });
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(WaitFor(
      [&] {
        std::scoped_lock lk(mu);
        return std::find(seen.begin(), seen.end(), NtripState::kStalled) !=
               seen.end();
      },
      2s));
  ASSERT_TRUE(
      WaitFor([&] { return caster.Connections() >= 2; }, 2s));  // reconnects
}

TEST(NtripTest, PasswordNotInStatusOrErrors) {
  FakeCaster caster(FakeCaster::Mode::kUnauthorized);
  VecSink sink;
  std::mutex mu;
  std::string reasons;
  NtripClient c(Opts(caster), sink);
  c.OnStatus([&](NtripState, NtripState, const std::string& r) {
    std::scoped_lock lk(mu);
    reasons += r;
  });
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(
      WaitFor([&] { return c.Status().state == NtripState::kAuthFailed; }, 2s));
  c.Stop();
  EXPECT_EQ(c.Status().last_error.find("s3cret"), std::string::npos);
  EXPECT_EQ(reasons.find("s3cret"), std::string::npos);
  EXPECT_NE(c.Status().last_error.find("401"), std::string::npos);
}

TEST(NtripTest, StopIsFastWhileConnecting) {
  VecSink sink;
  NtripOptions o;
  o.host = "192.0.2.1";  // TEST-NET-1: never answers
  o.mountpoint = "X";
  o.send_gga = false;
  o.connect_timeout_ms = 5000;
  NtripClient c(o, sink);
  ASSERT_TRUE(c.Start());
  std::this_thread::sleep_for(200ms);
  const auto t0 = std::chrono::steady_clock::now();
  c.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1200ms);
}

TEST(NtripTest, FetchSourcetableV2Chunked) {
  FakeCaster caster(FakeCaster::Mode::kSourcetable);
  std::string err;
  const auto table =
      NtripClient::FetchSourcetable("127.0.0.1", caster.Port(), 2, err);
  ASSERT_TRUE(table) << err;
  EXPECT_EQ(*table, FakeCaster::Sourcetable());
  const auto req = caster.Requests().at(0);
  EXPECT_NE(req.find("GET / HTTP/1.1"), std::string::npos) << req;
  EXPECT_NE(req.find("Ntrip-Version: Ntrip/2.0"), std::string::npos) << req;
}

TEST(NtripTest, FetchSourcetableV1) {
  FakeCaster caster(FakeCaster::Mode::kSourcetable);
  std::string err;
  const auto table =
      NtripClient::FetchSourcetable("127.0.0.1", caster.Port(), 1, err);
  ASSERT_TRUE(table) << err;
  EXPECT_EQ(*table, FakeCaster::Sourcetable());
  EXPECT_NE(caster.Requests().at(0).find("GET / HTTP/1.0"), std::string::npos);
  EXPECT_NE(caster.Requests().at(0).find("Connection: close"),
            std::string::npos);
}

TEST(NtripTest, UnknownMountpointReportsSourcetable) {
  FakeCaster caster(FakeCaster::Mode::kSourcetable);
  VecSink sink;
  NtripClient c(Opts(caster, 1), sink);
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(WaitFor(
      [&] {
        return c.Status().last_error.find("not found") != std::string::npos;
      },
      2s));
  EXPECT_NE(c.Status().last_error.find("NALPS"), std::string::npos);
}

TEST(NtripTest, BuildGgaFormat) {
  const std::string g = BuildGga(SomeFix());
  EXPECT_EQ(g.rfind("$GPGGA,100000.00,4637.8000000,N,00846.8000000,E,1,20,0.8,"
                    "1950.900,M,49.100,M,,*",
                    0),
            0u)
      << g;
  EXPECT_TRUE(g.ends_with("\r\n"));
}

TEST(NtripTest, Base64) {
  EXPECT_EQ(Base64("dennis:s3cret-pw"), "ZGVubmlzOnMzY3JldC1wdw==");
  EXPECT_EQ(Base64("a"), "YQ==");
  EXPECT_EQ(Base64(""), "");
}

}  // namespace
}  // namespace lgrtk
