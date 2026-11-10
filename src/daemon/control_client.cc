// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/control_client.h"

#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include "internal/unique_fd.h"
#include "internal/unix_socket.h"

namespace lgrtk::daemon {

using nlohmann::json;

std::optional<json> ControlRequest(const std::string& path, const json& request,
                                   int timeout_ms, std::string* err) {
  internal::UnixAddress a;
  if (!internal::MakeUnixAddress(path, a)) {
    *err = "socket path too long: " + path;
    return std::nullopt;
  }
  const internal::UniqueFd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd || connect(fd.Get(), a.Get(), a.len) != 0) {
    *err = "cannot connect to " + path + " (" +
           std::strerror(errno) +  // NOLINT(concurrency-mt-unsafe)
           "); is lgrtkd running?";
    return std::nullopt;
  }
  const std::string out = request.dump() + "\n";
  if (send(fd.Get(), out.data(), out.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(out.size())) {
    *err = "lgrtkd closed the control connection";
    return std::nullopt;
  }
  std::string in;
  const auto end =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (in.find('\n') == std::string::npos) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          end - std::chrono::steady_clock::now())
                          .count();
    pollfd p{fd.Get(), POLLIN, 0};
    if (left <= 0 || poll(&p, 1, static_cast<int>(left)) <= 0) {
      *err =
          "no answer from lgrtkd within " + std::to_string(timeout_ms) + " ms";
      return std::nullopt;
    }
    char buf[4096];
    const ssize_t n = recv(fd.Get(), buf, sizeof buf, 0);
    if (n <= 0) {
      *err = "lgrtkd closed the control connection";
      return std::nullopt;
    }
    in.append(buf, static_cast<size_t>(n));
  }
  json reply = json::parse(in.substr(0, in.find('\n')), nullptr, false);
  if (!reply.is_object()) {
    *err = "lgrtkd sent an unreadable reply";
    return std::nullopt;
  }
  return std::optional<json>(std::in_place, std::move(reply));
}

}  // namespace lgrtk::daemon
