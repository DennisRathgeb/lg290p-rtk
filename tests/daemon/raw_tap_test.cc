// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/raw_tap.h"

#include <poll.h>
#include <sys/socket.h>

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "daemon/net_util.h"
#include "test_util.h"

namespace lgrtk::daemon {
namespace {

std::string ReadBytes(int fd, size_t want, int timeout_ms = 2000) {
  std::string got;
  while (got.size() < want) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) {
      break;
    }
    char buf[4096];
    const ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) {
      break;
    }
    got.append(buf, static_cast<size_t>(n));
  }
  return got;
}

TEST(RawTapTest, ForwardsBytesVerbatim) {
  test::LoopThread lt;
  std::unique_ptr<RawTap> tap;
  int port = 0;
  lt.Sync([&] {
    tap = std::make_unique<RawTap>(lt.Loop());
    std::string err;
    ASSERT_TRUE(tap->Listen("127.0.0.1", 0, &err)) << err;
    port = tap->Port();
  });
  test::LineClient c;
  ASSERT_TRUE(c.ConnectTcp(port));
  ASSERT_TRUE(test::WaitFor([&] {
    size_t n = 0;
    lt.Sync([&] { n = tap->Clients(); });
    return n > 0;
  }));
  const auto rtcm = test::Rtcm({0x3E, 0xD0, 0x00, 0x0A, 0x00});
  std::string expect = test::Nmea("GNGGA,1") + "\r\n";
  expect.append(rtcm.begin(), rtcm.end());
  lt.Sync([&] {
    tap->Publish(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(expect.data()), expect.size()));
  });
  EXPECT_EQ(ReadBytes(c.Fd(), expect.size()), expect);
  lt.Sync([&] { tap.reset(); });
}

TEST(RawTapTest, InputFromClientIgnored) {
  test::LoopThread lt;
  std::unique_ptr<RawTap> tap;
  int port = 0;
  lt.Sync([&] {
    tap = std::make_unique<RawTap>(lt.Loop());
    std::string err;
    ASSERT_TRUE(tap->Listen("127.0.0.1", 0, &err));
    port = tap->Port();
  });
  test::LineClient c;
  ASSERT_TRUE(c.ConnectTcp(port));
  c.Write(std::string(100000, 'x'));  // no line limit on a write-only tap
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  size_t n = 0;
  lt.Sync([&] { n = tap->Clients(); });
  EXPECT_EQ(n, 1U);
  lt.Sync([&] { tap.reset(); });
}

}  // namespace
}  // namespace lgrtk::daemon
