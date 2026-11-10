// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// gpsd-compatible TCP server: VERSION on connect, then WATCH/POLL/DEVICES.
#ifndef LGRTK_SRC_DAEMON_GPSD_SERVER_H_
#define LGRTK_SRC_DAEMON_GPSD_SERVER_H_

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "daemon/event_loop.h"
#include "daemon/gpsd_json.h"
#include "lgrtk/fix.h"

namespace lgrtk::daemon {

/// Serves one device to gpsd clients. Every method runs on the loop thread.
class GpsdServer {
 public:
  /// `device` is the path clients see; `cap_bytes` bounds each client buffer.
  GpsdServer(EventLoop* loop, std::string device,
             size_t cap_bytes = 256 * 1024);
  /// Listens on host:port (0 = any free port).
  bool Listen(const std::string& host, int port, std::string* err);
  /// The bound port (after Listen).
  [[nodiscard]] int Port() const { return port_; }
  /// TPV (and GST when the fix has sigmas) to JSON watchers.
  void PublishFix(const Fix& fix);
  /// One NMEA sentence to NMEA watchers.
  void PublishNmea(std::string_view line);
  /// A SKY object (from SkyJson) to JSON watchers.
  void PublishSky(const std::string& sky_json);
  /// Device activated (receiver streaming) or lost; watchers get DEVICES on a
  /// change and, on loss, a TPV with mode 0.
  void SetActive(bool active, int baud);
  /// Connected clients.
  [[nodiscard]] size_t Clients() const { return clients_.Count(); }
  /// Clients dropped for not reading.
  [[nodiscard]] uint64_t Dropped() const { return clients_.Dropped(); }

 private:
  void OnLine(ClientSet::Id id, std::string_view line);
  void ToWatchers(bool json, bool nmea, std::string_view data);
  std::string PollJson() const;

  EventLoop* loop_;
  std::string device_;
  ClientSet clients_;
  std::map<ClientSet::Id, Watch> watch_;
  int port_ = 0;
  bool active_ = false;
  int baud_ = 0;
  std::optional<Fix> last_fix_;
  std::string last_sky_;
};

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_GPSD_SERVER_H_
