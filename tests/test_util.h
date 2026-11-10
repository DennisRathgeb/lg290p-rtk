// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_TESTS_TEST_UTIL_H_
#define LGRTK_TESTS_TEST_UTIL_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lgrtk/framer.h"

namespace lgrtk::test {

/// "$<body>*HH" with the correct checksum (no CR/LF).
inline std::string Nmea(std::string_view body) {
  unsigned sum = 0;
  for (char c : body) sum ^= static_cast<unsigned char>(c);
  static const char* const kHex = "0123456789ABCDEF";
  std::string s = "$";
  s += body;
  s += '*';
  s += kHex[(sum >> 4) & 0xF];
  s += kHex[sum & 0xF];
  return s;
}

/// A complete RTCM3 frame (0xD3, length, payload, CRC-24Q) around `payload`.
inline std::vector<uint8_t> Rtcm(const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> f{0xD3, static_cast<uint8_t>((payload.size() >> 8) & 0x03),
                         static_cast<uint8_t>(payload.size() & 0xFF)};
  f.insert(f.end(), payload.begin(), payload.end());
  const uint32_t crc = Crc24q(f);
  f.push_back(static_cast<uint8_t>(crc >> 16));
  f.push_back(static_cast<uint8_t>(crc >> 8));
  f.push_back(static_cast<uint8_t>(crc));
  return f;
}

inline std::vector<uint8_t> Bytes(std::string_view s) { return {s.begin(), s.end()}; }

}  // namespace lgrtk::test

#endif  // LGRTK_TESTS_TEST_UTIL_H_
