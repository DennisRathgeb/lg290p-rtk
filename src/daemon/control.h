// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// lgrtkd's control socket, server side (protocol: control_protocol.h).
#ifndef LGRTK_SRC_DAEMON_CONTROL_H_
#define LGRTK_SRC_DAEMON_CONTROL_H_

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "daemon/control_protocol.h"
#include "daemon/event_loop.h"

namespace lgrtk::daemon {

/// What the daemon does for each command; each returns the reply fields
/// (an `"error"` string in the result marks a failure). Run on the loop
/// thread, so they must not block for long.
struct ControlHandlers {
  std::function<nlohmann::json()> status;  ///< state and counters
  /// hot, warm, cold or soft (validated before the call)
  std::function<nlohmann::json(const std::string& kind)> reset;
  std::function<nlohmann::json()> apply_default;  ///< DefaultRoverConfig
};

/// Serves the control protocol on a Unix socket (mode 0660).
class ControlServer {
 public:
  /// Commands are dispatched to `handlers`.
  ControlServer(EventLoop* loop, ControlHandlers handlers);
  ~ControlServer();
  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;
  ControlServer(ControlServer&&) = delete;
  ControlServer& operator=(ControlServer&&) = delete;
  /// Listens on `path` (a stale socket file is replaced).
  bool Listen(const std::string& path, std::string* err);

 private:
  void OnLine(ClientSet::Id id, std::string_view line);
  ControlHandlers h_;
  ClientSet clients_;
  std::string path_;
};

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_CONTROL_H_
