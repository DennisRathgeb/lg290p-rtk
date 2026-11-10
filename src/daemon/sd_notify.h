// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// The systemd notification protocol (sd_notify(3)) without libsystemd.
#ifndef LGRTK_SRC_DAEMON_SD_NOTIFY_H_
#define LGRTK_SRC_DAEMON_SD_NOTIFY_H_

#include <cstdint>
#include <string_view>

namespace lgrtk::daemon {

/// Sends `state` ("READY=1", "STATUS=...", "WATCHDOG=1", "STOPPING=1") to
/// $NOTIFY_SOCKET; does nothing when the variable is unset. Abstract sockets
/// ("@name") are supported.
void SdNotify(std::string_view state);
/// Half of $WATCHDOG_USEC in ms (how often to send WATCHDOG=1); 0 if unset.
[[nodiscard]] int64_t WatchdogPeriodMs();

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_SD_NOTIFY_H_
