// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/event_loop.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "daemon/net_util.h"
#include "internal/unique_fd.h"

namespace lgrtk::daemon {
namespace {

using test::LineClient;
using test::LoopThread;

TEST(EventLoopTest, ClientLimitRefusesExtraClients) {
  LoopThread lt;
  std::unique_ptr<ClientSet> set;
  int port = 0;
  lt.Sync([&] {
    set = std::make_unique<ClientSet>(
        lt.Loop(), ClientSet::Handlers{},
        ClientSet::Options{.cap_bytes = 1024, .max_clients = 2});
    std::string err;
    internal::UniqueFd fd = ListenTcp("127.0.0.1", 0, &err);
    ASSERT_TRUE(fd) << err;
    port = BoundPort(fd.Get());
    set->AddListener(std::move(fd));
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
  ASSERT_TRUE(test::WaitFor([&] {  // until the server saw it
    lt.Sync([&] { n = set->Count(); });
    return n == 1;
  }));
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
  EXPECT_TRUE(test::WaitFor([&] { return n >= 5; }));
}

TEST(EventLoopTest, UnixListenerReplacesStaleSocket) {
  const std::string path =
      "/tmp/lgrtk_test_" + std::to_string(getpid()) + ".sock";
  std::string err;
  {
    const internal::UniqueFd a = ListenUnix(path, &err);
    ASSERT_TRUE(a) << err;
  }  // closed: leaves the socket file behind, like a crashed daemon
  const internal::UniqueFd b = ListenUnix(path, &err);
  ASSERT_TRUE(b) << err;
  unlink(path.c_str());
}

}  // namespace
}  // namespace lgrtk::daemon
