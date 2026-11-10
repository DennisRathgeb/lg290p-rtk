// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// Tests of the HTTP chunked decoder in src/internal/net.h.
#include "internal/net.h"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace lgrtk::internal {
namespace {

// Feeds `in` (optionally byte by byte) and collects the payload.
bool Decode(std::string_view in, std::string& out, bool bytewise = false) {
  Dechunker d;
  auto sink = [&](std::string_view s) { out += s; };
  if (!bytewise) {
    return d.Feed(in, sink);
  }
  for (size_t i = 0; i < in.size(); ++i) {
    if (!d.Feed(in.substr(i, 1), sink)) {
      return false;
    }
  }
  return true;
}

TEST(DechunkerTest, DecodesChunks) {
  std::string out;
  ASSERT_TRUE(Decode("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", out));
  EXPECT_EQ(out, "Wikipedia");
}

TEST(DechunkerTest, DecodesAcrossArbitrarySplits) {
  std::string out;
  ASSERT_TRUE(Decode("4\r\nWiki\r\na\r\n0123456789\r\n0\r\n\r\n", out, true));
  EXPECT_EQ(out, "Wiki0123456789");
}

TEST(DechunkerTest, AcceptsUpperCaseHexAndExtensions) {
  std::string out;
  ASSERT_TRUE(Decode("A;name=value\r\n0123456789\r\n0\r\n\r\n", out));
  EXPECT_EQ(out, "0123456789");
}

TEST(DechunkerTest, IgnoresDataAfterLastChunk) {
  std::string out;
  ASSERT_TRUE(Decode("1\r\nx\r\n0\r\n\r\ntrailing garbage", out));
  EXPECT_EQ(out, "x");
}

TEST(DechunkerTest, RejectsMalformedSizes) {
  for (const char* in :
       {"zz\r\n", "\r\n", "-1\r\nx", "+1\r\nx", "0x1\r\nx", "1 2\r\nxy"}) {
    std::string out;
    EXPECT_FALSE(Decode(in, out)) << in;
    EXPECT_TRUE(out.empty()) << in;
  }
}

TEST(DechunkerTest, RejectsOversizedSizes) {
  std::string out;
  // Does not fit in 64 bits.
  EXPECT_FALSE(Decode("10000000000000000\r\nx", out));
  // A size line longer than any sensible one.
  EXPECT_FALSE(Decode(std::string(Dechunker::kMaxSizeLine + 2, '1'), out));
  EXPECT_TRUE(out.empty());
}

TEST(ParseChunkSizeTest, Values) {
  size_t n = 0;
  ASSERT_TRUE(ParseChunkSize("1f", n));
  EXPECT_EQ(n, 31u);
  ASSERT_TRUE(ParseChunkSize(" 1F ; ext", n));
  EXPECT_EQ(n, 31u);
  ASSERT_TRUE(ParseChunkSize("ffffffffffffffff", n));
  EXPECT_EQ(n, ~size_t{0});
  EXPECT_FALSE(ParseChunkSize("", n));
  EXPECT_FALSE(ParseChunkSize(";ext", n));
}

TEST(ParseHttpStatusTest, StatusLines) {
  int status = 0;
  ASSERT_TRUE(ParseHttpStatus("HTTP/1.1 200 OK", status));
  EXPECT_EQ(status, 200);
  ASSERT_TRUE(ParseHttpStatus("HTTP/1.0 401 Unauthorized", status));
  EXPECT_EQ(status, 401);
  ASSERT_TRUE(ParseHttpStatus("HTTP/1.1 404", status));
  EXPECT_EQ(status, 404);
  EXPECT_FALSE(ParseHttpStatus("ICY 200 OK", status));
  EXPECT_FALSE(ParseHttpStatus("SOURCETABLE 200 OK", status));
  EXPECT_FALSE(ParseHttpStatus("HTTP/1.1 abc", status));
  EXPECT_FALSE(ParseHttpStatus("HTTP/x 200 OK", status));
  EXPECT_FALSE(ParseHttpStatus("", status));
  EXPECT_EQ(status, 404);  // unchanged on failure
}

}  // namespace
}  // namespace lgrtk::internal
