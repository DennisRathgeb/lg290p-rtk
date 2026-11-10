// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// Client side of lgrtkd's control socket (used by `lgrtk ctl`).
#ifndef LGRTK_SRC_DAEMON_CONTROL_CLIENT_H_
#define LGRTK_SRC_DAEMON_CONTROL_CLIENT_H_

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "daemon/control_protocol.h"

namespace lgrtk::daemon {

/// Sends `request` to the daemon at `path` and waits up to `timeout_ms` for
/// the reply. nullopt with `err` when the daemon cannot be reached or answers
/// garbage.
[[nodiscard]] std::optional<nlohmann::json> ControlRequest(
    const std::string& path, const nlohmann::json& request, int timeout_ms,
    std::string* err);

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_CONTROL_CLIENT_H_
