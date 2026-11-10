// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/framer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test_util.h"

namespace lgrtk {
namespace {

using test::Bytes;
using test::Nmea;
using test::Rtcm;

struct Collected {
  std::vector<std::string> nmea;
  std::vector<std::vector<uint8_t>> rtcm;
};

void FeedAll(StreamFramer& fr, const std::vector<uint8_t>& data, Collected& out,
             size_t split = 0) {
  auto cb = [&](const Frame& f) {
    if (f.kind == FrameKind::kNmea) {
      out.nmea.emplace_back(f.bytes.begin(), f.bytes.end());
    } else {
      out.rtcm.emplace_back(f.bytes.begin(), f.bytes.end());
    }
  };
  if (split == 0 || split >= data.size()) {
    fr.Feed(data, cb);
  } else {
    fr.Feed(std::span(data).first(split), cb);
    fr.Feed(std::span(data).subspan(split), cb);
  }
}

std::vector<uint8_t> Rtcm1005() {
  // Message number 1005 in the first 12 bits, rest arbitrary, 19 bytes total.
  std::vector<uint8_t> p(19, 0x11);
  p[0] = 0x3E;
  p[1] = 0xD0;
  return Rtcm(p);
}

TEST(FramerTest, Crc24qKnownVector) {
  const std::string s = "123456789";
  EXPECT_EQ(
      Crc24q(std::span(reinterpret_cast<const uint8_t*>(s.data()), s.size())),
      0xCDE703u);
}

TEST(FramerTest, SplitsAcrossEveryByteBoundary) {
  const std::string gga = Nmea("GNGGA,003508.100,,,,,0,00,99.99,,M,,M,,");
  std::vector<uint8_t> data = Bytes(gga + "\r\n");
  const auto rtcm = Rtcm1005();
  data.insert(data.end(), rtcm.begin(), rtcm.end());
  for (size_t split = 1; split < data.size(); ++split) {
    StreamFramer fr;
    Collected c;
    FeedAll(fr, data, c, split);
    ASSERT_EQ(c.nmea.size(), 1u) << "split " << split;
    ASSERT_EQ(c.rtcm.size(), 1u) << "split " << split;
    EXPECT_EQ(c.nmea[0], gga);
    EXPECT_EQ(c.rtcm[0], rtcm);
  }
}

TEST(FramerTest, ResyncsAfterGarbage) {
  StreamFramer fr;
  Collected c;
  const std::string gga = Nmea("GNGGA,003508.200,,,,,0,00,99.99,,M,,M,,");
  std::vector<uint8_t> data = {0x00, 0xFF, 0x13, 'x',
                               'y',  0xD3, 0x00};  // includes a fake RTCM start
  const auto line = Bytes(gga + "\r\n");
  data.insert(data.end(), line.begin(), line.end());
  FeedAll(fr, data, c);
  ASSERT_EQ(c.nmea.size(), 1u);
  EXPECT_EQ(c.nmea[0], gga);
  EXPECT_GT(fr.Stats().skipped_bytes, 0u);
}

TEST(FramerTest, DollarInsideRtcmPayload) {
  StreamFramer fr;
  Collected c;
  const std::string fake = "$GPGGA,1*00\r\n";
  std::vector<uint8_t> payload = {0x3E, 0xD0};
  payload.insert(payload.end(), fake.begin(), fake.end());
  FeedAll(fr, Rtcm(payload), c);
  EXPECT_EQ(c.nmea.size(), 0u);
  EXPECT_EQ(c.rtcm.size(), 1u);
}

TEST(FramerTest, BadCrcCounted) {
  StreamFramer fr;
  Collected c;
  auto f = Rtcm1005();
  f.back() ^= 0x01;
  const std::string gga = Nmea("GNGGA,1,,,,,0,00,,,M,,M,,");
  const auto line = Bytes(gga + "\r\n");
  f.insert(f.end(), line.begin(), line.end());
  FeedAll(fr, f, c);
  EXPECT_EQ(c.rtcm.size(), 0u);
  EXPECT_EQ(fr.Stats().bad_rtcm, 1u);
  EXPECT_EQ(c.nmea.size(), 1u);  // the following sentence survives
}

TEST(FramerTest, BadNmeaChecksumCounted) {
  StreamFramer fr;
  Collected c;
  FeedAll(fr, Bytes("$GNGGA,1,,,,,0,00,,,M,,M,,*00\r\n"), c);
  EXPECT_EQ(c.nmea.size(), 0u);
  EXPECT_EQ(fr.Stats().bad_nmea, 1u);
}

TEST(FramerTest, LongPqtmLineAccepted) {
  StreamFramer fr;
  Collected c;
  const std::string body = "PQTMTXT," + std::string(150, 'a');
  FeedAll(fr, Bytes(Nmea(body) + "\r\n"), c);
  EXPECT_EQ(c.nmea.size(), 1u);
}

TEST(FramerTest, OversizeLineDroppedAndCounted) {
  StreamFramer fr;
  Collected c;
  const std::string big = "$GNTXT," + std::string(400, 'a') + "\r\n";
  const std::string gga = Nmea("GNGGA,1,,,,,0,00,,,M,,M,,");
  FeedAll(fr, Bytes(big + gga + "\r\n"), c);
  EXPECT_EQ(fr.Stats().oversize, 1u);
  ASSERT_EQ(c.nmea.size(), 1u);
  EXPECT_EQ(c.nmea[0], gga);
}

TEST(FramerTest, NmeaChecksumOkHelper) {
  EXPECT_TRUE(NmeaChecksumOk("$PQTMVERNO*58"));
  EXPECT_FALSE(NmeaChecksumOk("$PQTMVERNO*59"));
  EXPECT_FALSE(NmeaChecksumOk("PQTMVERNO*58"));
}

}  // namespace
}  // namespace lgrtk
