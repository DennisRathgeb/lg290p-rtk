// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/gpsd_server.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "daemon/event_loop.h"
#include "daemon/net_util.h"
#include "test_util.h"

namespace lgrtk::daemon {
namespace {

using nlohmann::json;
using test::LineClient;
using test::LoopThread;

Fix SomeFix(uint32_t seq) {
  Fix f{};
  f.seq = seq;
  f.utc_s = 43200.0;
  f.date_ymd = 20261110;
  f.lat_deg = 46.63;
  f.lon_deg = 8.78;
  f.h_ell_m = 2000;
  f.geoid_sep_m = 49;
  f.type = FixType::kRtkFixed;
  f.has_position = true;
  f.sigma_source = SigmaSource::kGst;
  f.sigma_e_m = f.sigma_n_m = 0.01F;
  f.sigma_u_m = 0.02F;
  return f;
}

class GpsdServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(lt_.Ok());
    lt_.Sync([&] {
      server_ = std::make_unique<GpsdServer>(lt_.Loop(), "/dev/sim", cap_);
      std::string err;
      ASSERT_TRUE(server_->Listen("127.0.0.1", 0, &err)) << err;
      port_ = server_->Port();
    });
  }
  void TearDown() override {
    lt_.Sync([&] { server_.reset(); });
    lt_.Stop();
  }
  void Publish(const Fix& f) {
    lt_.Sync([&] { server_->PublishFix(f); });
  }
  // Connects and consumes the VERSION greeting.
  void Connect(LineClient* c) {
    ASSERT_TRUE(c->ConnectTcp(port_));
    auto v = c->ReadLine();
    ASSERT_TRUE(v);
    ASSERT_NE(v->find("\"VERSION\""), std::string::npos) << *v;
  }
  void WatchJson(LineClient* c) {
    c->Write("?WATCH={\"enable\":true,\"json\":true};\n");
    ASSERT_TRUE(c->ReadUntil("\"class\":\"WATCH\""));
  }
  size_t Clients() {
    size_t n = 0;
    lt_.Sync([&] { n = server_->Clients(); });
    return n;
  }

  size_t cap_ = 256 * 1024;
  LoopThread lt_;
  std::unique_ptr<GpsdServer> server_;
  int port_ = 0;
};

TEST_F(GpsdServerTest, VersionOnConnect) {
  LineClient c;
  Connect(&c);
}

TEST_F(GpsdServerTest, WatchStreamsTpv) {
  LineClient c;
  Connect(&c);
  c.Write("?WATCH={\"enable\":true,\"json\":true};\n");
  auto devices = c.ReadLine();
  ASSERT_TRUE(devices);
  EXPECT_NE(devices->find("\"DEVICES\""), std::string::npos) << *devices;
  auto watch = c.ReadLine();
  ASSERT_TRUE(watch);
  EXPECT_NE(watch->find("\"WATCH\""), std::string::npos) << *watch;
  Publish(SomeFix(1));
  auto tpv = c.ReadUntil("\"TPV\"");
  ASSERT_TRUE(tpv);
  EXPECT_EQ(json::parse(*tpv)["status"], 3);
  EXPECT_TRUE(c.ReadUntil("\"GST\""));
}

TEST_F(GpsdServerTest, NoStreamWithoutWatch) {
  LineClient c;
  Connect(&c);
  Publish(SomeFix(1));
  EXPECT_FALSE(c.ReadLine(300));
}

TEST_F(GpsdServerTest, WatchDisableStops) {
  LineClient c;
  Connect(&c);
  WatchJson(&c);
  c.Write("?WATCH={\"enable\":false};\n");
  ASSERT_TRUE(c.ReadUntil("\"enable\":false"));
  Publish(SomeFix(1));
  EXPECT_FALSE(c.ReadLine(300));
}

TEST_F(GpsdServerTest, PollReturnsLatest) {
  LineClient c;
  Connect(&c);
  lt_.Sync([&] { server_->SetActive(true, 460800); });
  Publish(SomeFix(7));
  c.Write("?POLL;\n");
  auto poll = c.ReadUntil("\"POLL\"");
  ASSERT_TRUE(poll);
  auto j = json::parse(*poll);
  EXPECT_EQ(j["active"], 1);
  ASSERT_EQ(j["tpv"].size(), 1U);
  EXPECT_EQ(j["tpv"][0]["mode"], 3);
  ASSERT_EQ(j["gst"].size(), 1U);
}

TEST_F(GpsdServerTest, PollBeforeAnyFix) {
  LineClient c;
  Connect(&c);
  c.Write("?POLL;\n");
  auto poll = c.ReadUntil("\"POLL\"");
  ASSERT_TRUE(poll);
  auto j = json::parse(*poll);
  EXPECT_EQ(j["active"], 0);
  EXPECT_TRUE(j["tpv"].empty());
}

TEST_F(GpsdServerTest, NmeaWatch) {
  LineClient c;
  Connect(&c);
  c.Write("?WATCH={\"enable\":true,\"nmea\":true};\n");
  ASSERT_TRUE(c.ReadUntil("\"WATCH\""));
  const std::string gga = test::Nmea("GNGGA,1");
  lt_.Sync([&] { server_->PublishNmea(gga + "\r\n"); });
  auto line = c.ReadLine();
  ASSERT_TRUE(line);
  EXPECT_EQ(*line, gga);
  Publish(SomeFix(1));  // JSON not requested
  EXPECT_FALSE(c.ReadLine(300));
}

TEST_F(GpsdServerTest, SkyToJsonWatchers) {
  LineClient c;
  Connect(&c);
  WatchJson(&c);
  lt_.Sync([&] { server_->PublishSky("{\"class\":\"SKY\"}\r\n"); });
  EXPECT_TRUE(c.ReadUntil("\"SKY\""));
}

TEST_F(GpsdServerTest, CommandsSplitAcrossReads) {
  LineClient c;
  Connect(&c);
  c.Write("?WAT");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  c.Write("CH={\"enable\":true,\"json\":true};\n?VERSION;\n?DEV");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  c.Write("ICES;\n");
  std::vector<std::string> classes;
  for (int i = 0; i < 4; ++i) {
    auto line = c.ReadLine();
    ASSERT_TRUE(line) << "after " << i;
    classes.push_back(json::parse(*line)["class"]);
  }
  EXPECT_EQ(classes, (std::vector<std::string>{"DEVICES", "WATCH", "VERSION",
                                               "DEVICES"}));
  EXPECT_FALSE(c.ReadLine(200));
}

TEST_F(GpsdServerTest, SeveralCommandsOnOneLine) {
  LineClient c;
  Connect(&c);
  c.Write("?VERSION;?DEVICES;\n");
  auto a = c.ReadLine();
  auto b = c.ReadLine();
  ASSERT_TRUE(a && b);
  EXPECT_EQ(json::parse(*a)["class"], "VERSION");
  EXPECT_EQ(json::parse(*b)["class"], "DEVICES");
}

TEST_F(GpsdServerTest, MalformedCommandGetsError) {
  LineClient c;
  Connect(&c);
  c.Write("?WATCH={\"enable\":tru};\n");
  auto e = c.ReadLine();
  ASSERT_TRUE(e);
  EXPECT_NE(e->find("\"ERROR\""), std::string::npos) << *e;
  c.Write("?FROB;\n");
  e = c.ReadLine();
  ASSERT_TRUE(e);
  EXPECT_NE(e->find("\"ERROR\""), std::string::npos) << *e;
  c.Write("?VERSION;\n");  // still connected
  EXPECT_TRUE(c.ReadUntil("\"VERSION\""));
}

TEST_F(GpsdServerTest, OverlongLineDropsClient) {
  LineClient c;
  Connect(&c);
  c.Write(std::string(10000, 'x'));
  EXPECT_TRUE(c.ClosedByPeer());
}

TEST_F(GpsdServerTest, LossSendsDevicesAndNoFix) {
  LineClient c;
  Connect(&c);
  lt_.Sync([&] { server_->SetActive(true, 460800); });
  WatchJson(&c);
  lt_.Sync([&] { server_->SetActive(false, 0); });
  auto d = c.ReadUntil("\"DEVICES\"");
  ASSERT_TRUE(d);
  EXPECT_EQ(json::parse(*d)["devices"][0]["activated"], 0);
  auto tpv = c.ReadUntil("\"TPV\"");
  ASSERT_TRUE(tpv);
  EXPECT_EQ(json::parse(*tpv)["mode"], 0);
  lt_.Sync([&] { server_->SetActive(true, 460800); });
  d = c.ReadUntil("\"DEVICES\"");
  ASSERT_TRUE(d);
  EXPECT_EQ(json::parse(*d)["devices"][0]["activated"], 1);
}

TEST_F(GpsdServerTest, ClientCountFollowsDisconnects) {
  {
    LineClient a;
    Connect(&a);
    LineClient b;
    Connect(&b);
    EXPECT_EQ(Clients(), 2U);
  }
  for (int i = 0; i < 100 && Clients() != 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(Clients(), 0U);
}

class GpsdServerSmallCapTest : public GpsdServerTest {
 protected:
  GpsdServerSmallCapTest() { cap_ = 16 * 1024; }
};

TEST_F(GpsdServerSmallCapTest, SlowClientIsDropped) {
  LineClient slow;
  ASSERT_TRUE(slow.ConnectTcp(port_, 4096));
  slow.Write("?WATCH={\"enable\":true,\"json\":true};\n");
  LineClient good;
  Connect(&good);
  WatchJson(&good);
  // Wait until the server has processed the slow client's WATCH.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  // Far more than kernel buffers plus the cap; `good` keeps reading.
  std::atomic<bool> reading{true};
  std::thread reader([&] {
    while (reading) {
      good.ReadLine(50);
    }
  });
  for (uint32_t i = 0; i < 40000; ++i) {
    Publish(SomeFix(i));
  }
  reading = false;
  reader.join();
  uint64_t dropped = 0;
  lt_.Sync([&] { dropped = server_->Dropped(); });
  EXPECT_EQ(dropped, 1U);
  EXPECT_EQ(Clients(), 1U);
  Publish(SomeFix(99999));
  EXPECT_TRUE(good.ReadUntil("\"TPV\""));
}

TEST(EventLoopTest, ClientLimitRefusesExtraClients) {
  LoopThread lt;
  std::unique_ptr<ClientSet> set;
  int port = 0;
  lt.Sync([&] {
    set = std::make_unique<ClientSet>(lt.Loop(), ClientSet::Handlers{}, 1024,
                                      true, 4096, /*max_clients=*/2);
    std::string err;
    const int fd = ListenTcp("127.0.0.1", 0, &err);
    ASSERT_GE(fd, 0) << err;
    port = BoundPort(fd);
    set->AddListener(fd);
  });
  LineClient a;
  LineClient b;
  LineClient c;
  ASSERT_TRUE(a.ConnectTcp(port));
  ASSERT_TRUE(b.ConnectTcp(port));
  ASSERT_TRUE(c.ConnectTcp(port));  // the kernel accepts; we close it
  EXPECT_TRUE(c.ClosedByPeer());
  size_t n = 0;
  lt.Sync([&] { n = set->Count(); });
  EXPECT_EQ(n, 2U);
  a.Close();
  LineClient d;  // room again after a client left
  ASSERT_TRUE(d.ConnectTcp(port));
  EXPECT_FALSE(d.ClosedByPeer(300));
  lt.Sync([&] { set.reset(); });
}

TEST(EventLoopTest, PostFromOtherThread) {
  LoopThread lt;
  ASSERT_TRUE(lt.Ok());
  std::atomic<int> n{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; ++i) {
    ts.emplace_back([&] {
      for (int k = 0; k < 100; ++k) {
        lt.Loop()->Post([&] { ++n; });
      }
    });
  }
  for (auto& t : ts) {
    t.join();
  }
  lt.Sync([] {});
  EXPECT_EQ(n.load(), 400);
}

TEST(EventLoopTest, TimerFires) {
  LoopThread lt;
  std::atomic<int> n{0};
  lt.Sync([&] { lt.Loop()->Every(10, [&] { ++n; }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_GE(n.load(), 5);
}

TEST(EventLoopTest, UnixListenerReplacesStaleSocket) {
  const std::string path =
      "/tmp/lgrtk_test_" + std::to_string(getpid()) + ".sock";
  std::string err;
  int a = ListenUnix(path, &err);
  ASSERT_GE(a, 0) << err;
  close(a);  // leaves the socket file behind, like a crashed daemon
  int b = ListenUnix(path, &err);
  ASSERT_GE(b, 0) << err;
  close(b);
  unlink(path.c_str());
}

}  // namespace
}  // namespace lgrtk::daemon
