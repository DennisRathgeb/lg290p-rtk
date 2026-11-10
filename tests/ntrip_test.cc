// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/ntrip.h"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "sim/fake_caster.h"

namespace lgrtk {
namespace {

using namespace std::chrono_literals;
using test::FakeCaster;

struct VecSink : CorrectionSink {
  std::mutex mu;
  std::vector<std::vector<uint8_t>> frames;
  bool Inject(std::span<const uint8_t> f) override {
    std::lock_guard lk(mu);
    frames.emplace_back(f.begin(), f.end());
    return true;
  }
  size_t Count() {
    std::lock_guard lk(mu);
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

template <class Pred>
bool WaitFor(Pred p, std::chrono::milliseconds t) {
  const auto end = std::chrono::steady_clock::now() + t;
  while (std::chrono::steady_clock::now() < end) {
    if (p()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return p();
}

TEST(NtripTest, V2ChunkedStreamsToSink) {
  FakeCaster caster(FakeCaster::Mode::kV2Chunked);
  VecSink sink;
  NtripClient c(Opts(caster), sink);
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(WaitFor([&] { return sink.Count() >= 3; }, 2s));
  EXPECT_EQ(c.Status().state, NtripState::kStreaming);
  {
    std::lock_guard lk(sink.mu);
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
    std::lock_guard lk(mu);
    seen.push_back(to);
  });
  ASSERT_TRUE(c.Start());
  ASSERT_TRUE(WaitFor(
      [&] {
        std::lock_guard lk(mu);
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
    std::lock_guard lk(mu);
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
