// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// lgrtkd's control protocol: one JSON object per line in each direction.
/// Requests: `{"cmd":"status"}`, `{"cmd":"reset","kind":"hot|warm|cold|soft"}`,
/// `{"cmd":"apply-default"}`. Replies carry `"ok"` and, on failure, `"error"`.
#ifndef LGRTK_SRC_DAEMON_CONTROL_PROTOCOL_H_
#define LGRTK_SRC_DAEMON_CONTROL_PROTOCOL_H_

namespace lgrtk::daemon {

/// Where lgrtkd listens unless `[control] sock` says otherwise.
inline constexpr char kDefaultControlSocket[] = "/run/lgrtkd/control.sock";

/// Request field naming the command.
inline constexpr char kFieldCmd[] = "cmd";
/// Request field of `reset`: hot, warm, cold or soft.
inline constexpr char kFieldKind[] = "kind";
/// Reply field: true on success.
inline constexpr char kFieldOk[] = "ok";
/// Reply field: the reason of a failure.
inline constexpr char kFieldError[] = "error";

/// Daemon and receiver state, counters.
inline constexpr char kCmdStatus[] = "status";
/// Restart the receiver (`kind`).
inline constexpr char kCmdReset[] = "reset";
/// Apply the default rover configuration.
inline constexpr char kCmdApplyDefault[] = "apply-default";

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_CONTROL_PROTOCOL_H_
