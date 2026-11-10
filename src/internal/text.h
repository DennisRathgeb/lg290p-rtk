// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file text.h
/// Small text helpers shared by the parsers, the daemon and the CLI (internal).
#ifndef LGRTK_SRC_INTERNAL_TEXT_H_
#define LGRTK_SRC_INTERNAL_TEXT_H_

#include <charconv>
#include <string_view>
#include <system_error>

namespace lgrtk::internal {

/// Parses all of `s` as a number (integer or floating point, per `T`).
/// Returns false on any trailing character, overflow or empty input; `out` is
/// unchanged then.
template <class T>
[[nodiscard]] bool ParseNumber(std::string_view s, T& out) noexcept {
  T v{};
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || r.ec != std::errc{} || r.ptr != s.data() + s.size()) {
    return false;
  }
  out = v;
  return true;
}

/// Like ParseNumber, but an empty field gives `empty_value` (NMEA fields are
/// often empty when there is no fix).
template <class T>
[[nodiscard]] bool ParseField(std::string_view s, T& out,
                              T empty_value = T{}) noexcept {
  if (s.empty()) {
    out = empty_value;
    return true;
  }
  return ParseNumber(s, out);
}

/// `s` without trailing CR and LF characters.
[[nodiscard]] constexpr std::string_view TrimLineEnd(
    std::string_view s) noexcept {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

/// XOR of the bytes of `body`: the NMEA checksum of the text between '$' and
/// '*'.
[[nodiscard]] constexpr unsigned NmeaXor(std::string_view body) noexcept {
  unsigned sum = 0;
  for (const char c : body) {
    sum ^= static_cast<unsigned char>(c);
  }
  return sum & 0xFFU;
}

}  // namespace lgrtk::internal

#endif  // LGRTK_SRC_INTERNAL_TEXT_H_
