// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// lgrtk CLI: a receiver session on the serial port (start, ask, wait) and
/// Ctrl-C handling.
#ifndef LGRTK_APPS_LGRTK_SESSION_H_
#define LGRTK_APPS_LGRTK_SESSION_H_

#include <functional>
#include <optional>
#include <vector>

#include "lgrtk/lg290p.h"
#include "lgrtk/nmea.h"
#include "lgrtk/receiver.h"

namespace lgrtk::cli {

/// Makes SIGINT and SIGTERM set the interrupted flag instead of killing.
void InstallSignalHandlers();
/// True after SIGINT or SIGTERM.
[[nodiscard]] bool Interrupted();

/// Waits `seconds` (nullopt: no limit), until Ctrl-C, or until `done`
/// returns true, whichever comes first.
void WaitInterruptible(std::optional<int> seconds,
                       const std::function<bool()>& done = {});

/// Starts `rx` and waits up to `timeout_s` until it streams. On failure the
/// reason is printed and `rx` is stopped again, so no callback runs after
/// this returns false.
[[nodiscard]] bool StartReceiver(Lg290pReceiver& rx, int timeout_s = 15);
/// Sends `cmd` and waits up to 5 s for its reply; nullopt on timeout.
std::optional<nmea::OwnedReply> Ask(Lg290pReceiver& rx, lg290p::Command cmd);
/// Sends commands one by one and prints the replies; returns an exit code
/// (kRuntime at the first missing or refused reply).
int RunCommands(Lg290pReceiver& rx, const std::vector<lg290p::Command>& cmds);

}  // namespace lgrtk::cli

#endif  // LGRTK_APPS_LGRTK_SESSION_H_
