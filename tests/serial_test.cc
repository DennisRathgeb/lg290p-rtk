// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/serial.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>
#include <thread>

#include "sim/sim_lg290p.h"

namespace lgrtk {
namespace {

std::string ReadFor(SerialPort& p, int ms) {
  std::string got;
  std::array<uint8_t, 512> buf{};
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    const int n = p.Read(buf, 50);
    if (n < 0) break;
    got.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
  }
  return got;
}

TEST(SerialTest, ReadsFromSim) {
  test::SimLg290p sim;
  std::string err;
  auto port = SerialPort::Open(sim.path(), 460800, err);
  ASSERT_TRUE(port) << err;
  const std::string got = ReadFor(*port, 350);
  EXPECT_NE(got.find("$GNGGA,"), std::string::npos);
  EXPECT_NE(got.find("$GNRMC,"), std::string::npos);
}

TEST(SerialTest, WriteReachesSimAndGetsReply) {
  test::SimLg290p sim;
  std::string err;
  auto port = SerialPort::Open(sim.path(), 460800, err);
  ASSERT_TRUE(port) << err;
  const std::string cmd = "$PQTMVERNO*58\r\n";
  size_t written = 0;
  ASSERT_TRUE(port->WriteSome(std::span(reinterpret_cast<const uint8_t*>(cmd.data()), cmd.size()),
                              written));
  EXPECT_EQ(written, cmd.size());
  EXPECT_NE(ReadFor(*port, 300).find("$PQTMVERNO,LG290P03AANR01A06S"), std::string::npos);
}

TEST(SerialTest, HangupReturnsMinusOne) {
  test::SimLg290p sim;
  std::string err;
  auto port = SerialPort::Open(sim.path(), 460800, err);
  ASSERT_TRUE(port) << err;
  sim.Unplug();
  std::array<uint8_t, 256> buf{};
  int n = 0;
  for (int i = 0; i < 20 && n >= 0; ++i) n = port->Read(buf, 50);
  EXPECT_EQ(n, -1);
}

TEST(SerialTest, SecondOpenFailsExclusive) {
  test::SimLg290p sim;
  std::string err;
  auto first = SerialPort::Open(sim.path(), 460800, err);
  ASSERT_TRUE(first) << err;
  auto second = SerialPort::Open(sim.path(), 460800, err);
  EXPECT_FALSE(second);
  EXPECT_NE(err.find("busy"), std::string::npos) << err;
}

TEST(SerialTest, MissingPortFailsWithReason) {
  std::string err;
  EXPECT_FALSE(SerialPort::Open("/dev/does-not-exist-lgrtk", 460800, err));
  EXPECT_NE(err.find("/dev/does-not-exist-lgrtk"), std::string::npos);
}

TEST(SerialTest, UnsupportedBaudRejected) {
  test::SimLg290p sim;
  std::string err;
  EXPECT_FALSE(SerialPort::Open(sim.path(), 12345, err));
}

}  // namespace
}  // namespace lgrtk
