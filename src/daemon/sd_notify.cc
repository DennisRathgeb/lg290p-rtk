// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/sd_notify.h"

#include <sys/socket.h>

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "internal/unique_fd.h"
#include "internal/unix_socket.h"

namespace lgrtk::daemon {

void SdNotify(std::string_view state) {
  const char* env =
      std::getenv("NOTIFY_SOCKET");  // NOLINT(concurrency-mt-unsafe)
  if (env == nullptr || env[0] == '\0') {
    return;
  }
  const std::string_view path(env);
  internal::UnixAddress a;
  // Only absolute paths and abstract names ("@...") are valid.
  if ((path[0] != '/' && path[0] != '@') ||
      !internal::MakeUnixAddress(path, a)) {
    return;
  }
  const internal::UniqueFd fd(socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (!fd) {
    return;
  }
  (void)!sendto(fd.Get(), state.data(), state.size(), MSG_NOSIGNAL, a.Get(),
                a.len);
}

int64_t WatchdogPeriodMs() {
  const char* env =
      std::getenv("WATCHDOG_USEC");  // NOLINT(concurrency-mt-unsafe)
  if (env == nullptr) {
    return 0;
  }
  int64_t usec = 0;
  const char* end = env + std::strlen(env);
  const auto r = std::from_chars(env, end, usec);
  if (r.ec != std::errc() || r.ptr != end || usec <= 0) {
    return 0;
  }
  return usec / 2000;
}

}  // namespace lgrtk::daemon
