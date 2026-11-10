// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// lgrtkd configuration: the TOML file (`/etc/lgrtk/lgrtkd.toml`) and the
/// NTRIP password sources.
#ifndef LGRTK_SRC_DAEMON_CONFIG_H_
#define LGRTK_SRC_DAEMON_CONFIG_H_

#include <optional>
#include <string>
#include <string_view>

#include "daemon/control_protocol.h"
#include "internal/host_port.h"
#include "lgrtk/receiver.h"

namespace lgrtk::daemon {

/// The `[ntrip]` table.
struct NtripSection {
  bool enabled = false;       ///< connect to a caster at all
  std::string host;           ///< caster host
  int port = 2101;            ///< caster port
  std::string mountpoint;     ///< mountpoint without '/'
  std::string user;           ///< user name
  std::string password_file;  ///< file holding the password (one line)
  int version = 2;            ///< NTRIP 1 or 2
  bool send_gga = true;       ///< upload the rover position
};

/// Everything lgrtkd reads from its configuration file.
struct DaemonConfig {
  ReceiverOptions receiver;  ///< `[receiver]` port, baud, apply_config
  std::string gpsd_listen = "127.0.0.1:2947";  ///< `[gpsd] listen`
  std::string raw_listen;                      ///< `[raw] listen`; "" = off
  std::string chrony_sock;                     ///< `[chrony] sock`; "" = off
  std::string control_sock = kDefaultControlSocket;  ///< `[control] sock`
  std::string device_name;  ///< gpsd "device"; defaults to receiver.port
  NtripSection ntrip;       ///< `[ntrip]`
};

/// Parses TOML text. Unknown tables or keys, wrong types and invalid values
/// are errors; `err` names the offending key (or the line of a syntax error).
[[nodiscard]] std::optional<DaemonConfig> ParseConfig(std::string_view toml,
                                                      std::string* err);
/// Reads and parses a file.
[[nodiscard]] std::optional<DaemonConfig> LoadConfig(const std::string& path,
                                                     std::string* err);
/// The NTRIP password: from `password_file` if set, else from
/// `$CREDENTIALS_DIRECTORY/ntrip-password` (systemd `ImportCredential=`) if it
/// exists, else "". Trailing CR/LF is stripped. nullopt if a named file
/// cannot be read.
[[nodiscard]] std::optional<std::string> ReadNtripPassword(
    const NtripSection& ntrip, std::string* err);
/// "host:port" or "[v6]:port" (see internal::ParseHostPort).
using internal::ParseHostPort;

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_CONFIG_H_
