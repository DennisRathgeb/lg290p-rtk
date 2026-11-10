// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/control.h"

#include <unistd.h>

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "daemon/control_client.h"
#include "daemon/net_util.h"

namespace lgrtk::daemon {
namespace {

using nlohmann::json;

class ControlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = "/tmp/lgrtk_ctl_" + std::to_string(getpid()) + ".sock";
    ControlHandlers h;
    h.status = [] { return json{{"state", "streaming"}, {"epochs", 12}}; };
    h.reset = [this](const std::string& kind) {
      last_reset_ = kind;
      return json::object();
    };
    h.apply_default = [] { return json{{"changed", 2}}; };
    lt_.Sync([&] {
      server_ = std::make_unique<ControlServer>(lt_.Loop(), h);
      std::string err;
      ASSERT_TRUE(server_->Listen(path_, &err)) << err;
    });
  }
  void TearDown() override {
    lt_.Sync([&] { server_.reset(); });
    lt_.Stop();
    unlink(path_.c_str());
  }
  json Ask(const json& req) {
    std::string err;
    auto r = ControlRequest(path_, req, 2000, &err);
    EXPECT_TRUE(r) << err;
    return r ? *r : json();
  }

  test::LoopThread lt_;
  std::unique_ptr<ControlServer> server_;
  std::string path_;
  std::string last_reset_;
};

TEST_F(ControlTest, StatusRoundTrip) {
  json r = Ask({{"cmd", "status"}});
  EXPECT_EQ(r["ok"], true);
  EXPECT_EQ(r["state"], "streaming");
  EXPECT_EQ(r["epochs"], 12);
}

TEST_F(ControlTest, ResetPassesKind) {
  json r = Ask({{"cmd", "reset"}, {"kind", "hot"}});
  EXPECT_EQ(r["ok"], true);
  EXPECT_EQ(last_reset_, "hot");
}

TEST_F(ControlTest, ResetNeedsKnownKind) {
  json r = Ask({{"cmd", "reset"}, {"kind", "lukewarm"}});
  EXPECT_EQ(r["ok"], false);
  EXPECT_EQ(last_reset_, "");
}

TEST_F(ControlTest, FactoryResetRefused) {
  json r = Ask({{"cmd", "reset"}, {"kind", "factory"}});
  EXPECT_EQ(r["ok"], false);
  EXPECT_NE(r["error"].get<std::string>().find("factory"), std::string::npos);
  EXPECT_EQ(last_reset_, "");
}

TEST_F(ControlTest, ApplyDefault) {
  json r = Ask({{"cmd", "apply-default"}});
  EXPECT_EQ(r["ok"], true);
  EXPECT_EQ(r["changed"], 2);
}

TEST_F(ControlTest, UnknownCommand) {
  json r = Ask({{"cmd", "self-destruct"}});
  EXPECT_EQ(r["ok"], false);
  EXPECT_TRUE(r["error"].is_string());
}

TEST_F(ControlTest, BadJson) {
  test::LineClient c;
  ASSERT_TRUE(c.ConnectUnix(path_));
  c.Write("{not json\n");
  auto line = c.ReadLine();
  ASSERT_TRUE(line);
  json r = json::parse(*line);
  EXPECT_EQ(r["ok"], false);
}

TEST_F(ControlTest, NoDaemonIsAClearError) {
  std::string err;
  EXPECT_FALSE(ControlRequest("/tmp/lgrtk_nobody_here.sock",
                              json{{"cmd", "status"}}, 500, &err));
  EXPECT_NE(err.find("lgrtkd"), std::string::npos) << err;
}

}  // namespace
}  // namespace lgrtk::daemon
