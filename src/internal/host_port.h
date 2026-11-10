// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file host_port.h
/// "host:port" endpoints and one-line secret files, shared by lgrtkd's
/// configuration and the CLI (internal, not installed).
#ifndef LGRTK_SRC_INTERNAL_HOST_PORT_H_
#define LGRTK_SRC_INTERNAL_HOST_PORT_H_

#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "internal/text.h"

namespace lgrtk::internal {

/// Highest TCP port number.
inline constexpr int kMaxPort = 65535;

/// Splits "host:port" or "[v6]:port"; false for an empty host or a port
/// outside 1..65535.
[[nodiscard]] inline bool ParseHostPort(std::string_view s, std::string* host,
                                        int* port) {
  std::string_view h;
  std::string_view p;
  if (!s.empty() && s.front() == '[') {
    const auto close = s.find("]:");
    if (close == std::string_view::npos) {
      return false;
    }
    h = s.substr(1, close - 1);
    p = s.substr(close + 2);
  } else {
    const auto colon = s.rfind(':');
    if (colon == std::string_view::npos) {
      return false;
    }
    h = s.substr(0, colon);
    p = s.substr(colon + 1);
  }
  int v = 0;
  if (h.empty() || p.empty() || p.front() < '0' || p.front() > '9' ||
      !ParseNumber(p, v) || v < 1 || v > kMaxPort) {
    return false;
  }
  *host = std::string(h);
  *port = v;
  return true;
}

/// The first line of the file at `path` without trailing CR/LF ("" for an
/// empty file); nullopt and `err` when the file cannot be read.
[[nodiscard]] inline std::optional<std::string> ReadFirstLine(
    const std::string& path, std::string* err) {
  std::ifstream in(path);
  if (!in) {
    *err = "cannot read " + path;
    return std::nullopt;
  }
  std::string line;
  std::getline(in, line);
  return std::string(TrimLineEnd(line));
}

}  // namespace lgrtk::internal

#endif  // LGRTK_SRC_INTERNAL_HOST_PORT_H_
