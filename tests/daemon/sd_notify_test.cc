// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/sd_notify.h"

#include <gtest/gtest.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>

namespace lgrtk::daemon {
namespace {

TEST(SdNotifyTest, SendsState) {
  const std::string path = "/tmp/lgrtk_notify_" + std::to_string(getpid());
  unlink(path.c_str());
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
  ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
  setenv("NOTIFY_SOCKET", path.c_str(), 1);
  SdNotify("READY=1\nSTATUS=streaming");
  unsetenv("NOTIFY_SOCKET");
  pollfd p{fd, POLLIN, 0};
  ASSERT_EQ(poll(&p, 1, 1000), 1);
  char buf[256] = {};
  const ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)),
            "READY=1\nSTATUS=streaming");
  close(fd);
  unlink(path.c_str());
}

TEST(SdNotifyTest, AbstractSocket) {
  const std::string name = "lgrtk_notify_abs_" + std::to_string(getpid());
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path + 1, name.data(), name.size());
  const auto len =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&a), len), 0);
  setenv("NOTIFY_SOCKET", ("@" + name).c_str(), 1);
  SdNotify("WATCHDOG=1");
  unsetenv("NOTIFY_SOCKET");
  pollfd p{fd, POLLIN, 0};
  ASSERT_EQ(poll(&p, 1, 1000), 1);
  char buf[64] = {};
  EXPECT_EQ(recv(fd, buf, sizeof buf, 0), 10);
  close(fd);
}

TEST(SdNotifyTest, NoSocketIsNoOp) {
  unsetenv("NOTIFY_SOCKET");
  SdNotify("READY=1");  // must not crash or block
}

TEST(SdNotifyTest, WatchdogPeriod) {
  unsetenv("WATCHDOG_USEC");
  EXPECT_EQ(WatchdogPeriodMs(), 0);
  setenv("WATCHDOG_USEC", "10000000", 1);
  EXPECT_EQ(WatchdogPeriodMs(), 5000);
  setenv("WATCHDOG_USEC", "garbage", 1);
  EXPECT_EQ(WatchdogPeriodMs(), 0);
  unsetenv("WATCHDOG_USEC");
}

}  // namespace
}  // namespace lgrtk::daemon
