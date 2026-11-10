// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/chrony.h"

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "daemon/gpsd_json.h"

namespace lgrtk::daemon {
namespace {

Fix NoonFix() {
  Fix f{};
  f.date_ymd = 20261110;
  f.utc_s = 43200.0;
  f.has_position = true;
  f.type = FixType::kSingle;
  return f;
}

int BindDgram(const std::string& path) {
  unlink(path.c_str());
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
  EXPECT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
  return fd;
}

TEST(ChronyTest, SampleLayout) {
  EXPECT_EQ(sizeof(ChronySample), 40U);
  auto s = MakeChronySample(NoonFix(), *FixUnixNs(NoonFix()));
  ASSERT_TRUE(s);
  EXPECT_EQ(s->magic, 0x534f434b);
  EXPECT_EQ(s->pulse, 0);
  EXPECT_EQ(s->leap, 0);
}

TEST(ChronyTest, OffsetSign) {
  const int64_t fix_ns = *FixUnixNs(NoonFix());
  const int64_t arrival = fix_ns + 80000000;  // read 80 ms after the epoch
  auto s = MakeChronySample(NoonFix(), arrival);
  ASSERT_TRUE(s);
  EXPECT_NEAR(s->offset, -0.080, 1e-9);
  EXPECT_EQ(static_cast<int64_t>(s->tv.tv_sec), arrival / 1000000000);
  EXPECT_EQ(static_cast<int64_t>(s->tv.tv_usec), (arrival / 1000) % 1000000);
}

TEST(ChronyTest, NoSampleWithoutDate) {
  Fix f = NoonFix();
  f.date_ymd = 0;
  EXPECT_FALSE(MakeChronySample(f, 1));
}

TEST(ChronyTest, NoSampleWithoutFix) {
  Fix f = NoonFix();
  f.type = FixType::kNone;
  f.has_position = false;
  EXPECT_FALSE(MakeChronySample(f, *FixUnixNs(NoonFix())));
}

TEST(ChronyTest, NoSampleWhenClockFarOff) {
  // More than 0.5 s apart: the second of the sentence is ambiguous.
  EXPECT_FALSE(
      MakeChronySample(NoonFix(), *FixUnixNs(NoonFix()) + 2000000000LL));
}

TEST(ChronyTest, SendsToSocket) {
  const std::string path = "/tmp/lgrtk_chrony_" + std::to_string(getpid());
  const int fd = BindDgram(path);
  ChronySock sock(path);
  auto s = MakeChronySample(NoonFix(), *FixUnixNs(NoonFix()) + 50000000);
  ASSERT_TRUE(s);
  sock.Send(*s);
  pollfd p{fd, POLLIN, 0};
  ASSERT_EQ(poll(&p, 1, 1000), 1);
  ChronySample got{};
  EXPECT_EQ(recv(fd, &got, sizeof got, 0), 40);
  EXPECT_EQ(got.magic, 0x534f434b);
  EXPECT_NEAR(got.offset, -0.050, 1e-9);
  close(fd);
  unlink(path.c_str());
}

TEST(ChronyTest, SilentWithoutChrony) {
  ChronySock sock("/tmp/lgrtk_no_such_chrony_socket");
  sock.Send(*MakeChronySample(NoonFix(), *FixUnixNs(NoonFix())));
  EXPECT_EQ(sock.Sent(), 0U);
}

}  // namespace
}  // namespace lgrtk::daemon
