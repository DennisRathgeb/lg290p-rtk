// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// TCP tap that streams the untouched receiver bytes (NMEA + RTCM) to every
/// client, for loggers and RTKLIB `str2str`. Clients only receive.
#ifndef LGRTK_SRC_DAEMON_RAW_TAP_H_
#define LGRTK_SRC_DAEMON_RAW_TAP_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "daemon/event_loop.h"

namespace lgrtk::daemon {

/// Raw byte server; every method runs on the loop thread except Clients().
class RawTap {
 public:
  /// `cap_bytes` bounds each client buffer (a client that falls behind is
  /// dropped).
  explicit RawTap(EventLoop* loop, size_t cap_bytes = 1024 * 1024);
  /// Listens on host:port (0 = any free port).
  bool Listen(const std::string& host, int port, std::string* err);
  /// The bound port.
  [[nodiscard]] int Port() const { return port_; }
  /// Sends `bytes` to every client.
  void Publish(std::span<const uint8_t> bytes);
  /// Connected clients.
  [[nodiscard]] size_t Clients() const { return clients_.Count(); }

 private:
  ClientSet clients_;
  int port_ = 0;
};

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_RAW_TAP_H_
