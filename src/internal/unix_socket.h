// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file unix_socket.h
/// Unix-domain socket addresses (internal, not installed).
#ifndef LGRTK_SRC_INTERNAL_UNIX_SOCKET_H_
#define LGRTK_SRC_INTERNAL_UNIX_SOCKET_H_

#include <sys/socket.h>
#include <sys/un.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace lgrtk::internal {

/// A filled-in `sockaddr_un` and its length.
struct UnixAddress {
  sockaddr_un addr{};
  socklen_t len = 0;

  [[nodiscard]] const sockaddr* Get() const noexcept {
    return reinterpret_cast<const sockaddr*>(&addr);
  }
};

/// What a leading '@' in a socket path means.
enum class AtPrefix : uint8_t {
  kFileName,  ///< part of a filesystem path (configured sockets)
  kAbstract,  ///< an abstract socket, as in $NOTIFY_SOCKET
};

/// Builds the address for `path`. False when the path is empty or too long.
[[nodiscard]] inline bool MakeUnixAddress(
    std::string_view path, UnixAddress& out,
    AtPrefix at = AtPrefix::kFileName) noexcept {
  out = UnixAddress{};
  out.addr.sun_family = AF_UNIX;
  // Abstract names need no terminating NUL; filesystem paths do.
  const bool abstract =
      at == AtPrefix::kAbstract && !path.empty() && path.front() == '@';
  const size_t cap = sizeof out.addr.sun_path - (abstract ? 0 : 1);
  if (path.empty() || path.size() > cap) {
    return false;
  }
  std::memcpy(out.addr.sun_path, path.data(), path.size());
  if (abstract) {
    out.addr.sun_path[0] = '\0';
  }
  out.len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                   path.size() + (abstract ? 0 : 1));
  return true;
}

}  // namespace lgrtk::internal

#endif  // LGRTK_SRC_INTERNAL_UNIX_SOCKET_H_
