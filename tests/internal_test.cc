// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// Tests of the internal helpers in src/internal/.
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <utility>

#include "internal/clock.h"
#include "internal/text.h"
#include "internal/unique_fd.h"
#include "internal/unix_socket.h"

namespace lgrtk::internal {
namespace {

bool IsOpen(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

TEST(UniqueFdTest, ClosesOnDestruction) {
  int raw = -1;
  {
    UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_TRUE(fd);
    raw = fd.Get();
    EXPECT_TRUE(IsOpen(raw));
  }
  EXPECT_FALSE(IsOpen(raw));
}

TEST(UniqueFdTest, MoveTransfersOwnership) {
  UniqueFd a(::open("/dev/null", O_RDONLY | O_CLOEXEC));
  const int raw = a.Get();
  UniqueFd b = std::move(a);
  EXPECT_FALSE(a.Valid());  // NOLINT(bugprone-use-after-move)
  EXPECT_EQ(b.Get(), raw);
  UniqueFd c;
  c = std::move(b);
  EXPECT_EQ(c.Get(), raw);
  EXPECT_TRUE(IsOpen(raw));
}

TEST(UniqueFdTest, ReleaseDoesNotClose) {
  UniqueFd a(::open("/dev/null", O_RDONLY | O_CLOEXEC));
  const int raw = a.Release();
  EXPECT_FALSE(a);
  EXPECT_TRUE(IsOpen(raw));
  ::close(raw);
}

TEST(UniqueFdTest, ResetClosesThePrevious) {
  UniqueFd a(::open("/dev/null", O_RDONLY | O_CLOEXEC));
  const int first = a.Get();
  a.Reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
  EXPECT_FALSE(IsOpen(first) && first != a.Get());
  a.Reset();
  EXPECT_FALSE(a);
}

TEST(ClockTest, MonotonicNsAdvances) {
  const int64_t a = MonotonicNs();
  const int64_t b = MonotonicNs();
  EXPECT_GT(a, 0);
  EXPECT_GE(b, a);
  EXPECT_EQ(MsToNs(250), 250 * kNsPerMs);
}

TEST(TextTest, ParseNumberWholeStringOnly) {
  int i = 7;
  EXPECT_TRUE(ParseNumber("42", i));
  EXPECT_EQ(i, 42);
  EXPECT_FALSE(ParseNumber("42x", i));
  EXPECT_FALSE(ParseNumber("", i));
  EXPECT_FALSE(ParseNumber("99999999999", i));  // overflow
  EXPECT_EQ(i, 42);                             // unchanged on failure
  double d = 0;
  EXPECT_TRUE(ParseNumber("-1.5e2", d));
  EXPECT_DOUBLE_EQ(d, -150.0);
}

TEST(TextTest, ParseFieldEmptyGivesDefault) {
  int i = 0;
  EXPECT_TRUE(ParseField("", i, -1));
  EXPECT_EQ(i, -1);
  EXPECT_TRUE(ParseField("3", i, -1));
  EXPECT_EQ(i, 3);
  EXPECT_FALSE(ParseField("3a", i, -1));
}

TEST(TextTest, TrimLineEnd) {
  EXPECT_EQ(TrimLineEnd("abc\r\n"), "abc");
  EXPECT_EQ(TrimLineEnd("abc\n\n"), "abc");
  EXPECT_EQ(TrimLineEnd("abc"), "abc");
  EXPECT_EQ(TrimLineEnd("\r\n"), "");
}

TEST(TextTest, NmeaXor) {
  // $PQTMHOT*4B (protocol specification example)
  EXPECT_EQ(NmeaXor("PQTMHOT"), 0x4BU);
  EXPECT_EQ(NmeaXor(""), 0U);
}

TEST(UnixSocketTest, FilesystemPath) {
  UnixAddress a;
  ASSERT_TRUE(MakeUnixAddress("/run/x.sock", a));
  EXPECT_EQ(std::string(a.addr.sun_path), "/run/x.sock");
  EXPECT_EQ(a.len, offsetof(sockaddr_un, sun_path) + 12);
}

TEST(UnixSocketTest, AbstractName) {
  UnixAddress a;
  ASSERT_TRUE(MakeUnixAddress("@notify", a));
  EXPECT_EQ(a.addr.sun_path[0], '\0');
  EXPECT_EQ(std::string(a.addr.sun_path + 1, 6), "notify");
  EXPECT_EQ(a.len, offsetof(sockaddr_un, sun_path) + 7);
}

TEST(UnixSocketTest, RejectsEmptyAndTooLong) {
  UnixAddress a;
  EXPECT_FALSE(MakeUnixAddress("", a));
  EXPECT_FALSE(MakeUnixAddress(std::string(sizeof a.addr.sun_path, 'x'), a));
  EXPECT_TRUE(MakeUnixAddress(std::string(sizeof a.addr.sun_path - 1, 'x'), a));
}

}  // namespace
}  // namespace lgrtk::internal
