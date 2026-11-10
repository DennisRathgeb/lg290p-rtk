// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/sd_notify.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lgrtk::daemon {

void SdNotify(std::string_view state) {
  const char* env =
      std::getenv("NOTIFY_SOCKET");  // NOLINT(concurrency-mt-unsafe)
  if (env == nullptr || env[0] == '\0') {
    return;
  }
  const std::string path(env);
  sockaddr_un a{};
  if (path.size() >= sizeof a.sun_path || (path[0] != '/' && path[0] != '@')) {
    return;
  }
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.data(), path.size());
  auto len =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());
  if (path[0] == '@') {
    a.sun_path[0] = '\0';  // abstract namespace
  } else {
    ++len;  // include the terminating NUL
  }
  const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return;
  }
  (void)!sendto(fd, state.data(), state.size(), MSG_NOSIGNAL,
                reinterpret_cast<sockaddr*>(&a), len);
  close(fd);
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
