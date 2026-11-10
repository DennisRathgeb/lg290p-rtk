// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/control.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

namespace lgrtk::daemon {
namespace {

using nlohmann::json;

json Fail(const std::string& msg) {
  return json{{"ok", false}, {"error", msg}};
}

// Merges the handler's fields under "ok"; an "error" field marks failure.
json Reply(json fields) {
  json r{{"ok", !fields.contains("error")}};
  if (fields.is_object()) {
    r.update(fields);
  }
  return r;
}

}  // namespace

ControlServer::ControlServer(EventLoop* loop, ControlHandlers handlers)
    : h_(std::move(handlers)),
      clients_(loop, ClientSet::Handlers{
                         nullptr,
                         [this](ClientSet::Id id, std::string_view line) {
                           OnLine(id, line);
                         },
                         nullptr}) {}

ControlServer::~ControlServer() {
  if (!path_.empty()) {
    unlink(path_.c_str());
  }
}

bool ControlServer::Listen(const std::string& path, std::string* err) {
  const int fd = ListenUnix(path, err);
  if (fd < 0) {
    return false;
  }
  path_ = path;
  clients_.AddListener(fd);
  return true;
}

void ControlServer::OnLine(ClientSet::Id id, std::string_view line) {
  const json req = json::parse(line, nullptr, false);
  json reply;
  if (!req.is_object() || !req.contains("cmd") || !req["cmd"].is_string()) {
    reply = Fail("expected {\"cmd\": ...}");
  } else {
    const std::string cmd = req["cmd"].get<std::string>();
    if (cmd == "status" && h_.status) {
      reply = Reply(h_.status());
    } else if (cmd == "reset" && h_.reset) {
      const std::string kind = req.contains("kind") && req["kind"].is_string()
                                   ? req["kind"].get<std::string>()
                                   : "";
      if (kind == "factory") {
        reply = Fail(
            "factory reset is refused over the control socket; stop lgrtkd "
            "and run 'lgrtk reset factory --yes'");
      } else if (kind == "hot" || kind == "warm" || kind == "cold" ||
                 kind == "soft") {
        reply = Reply(h_.reset(kind));
      } else {
        reply = Fail("reset kind must be hot, warm, cold or soft");
      }
    } else if (cmd == "apply-default" && h_.apply_default) {
      reply = Reply(h_.apply_default());
    } else {
      reply = Fail("unknown command '" + cmd + "'");
    }
  }
  clients_.Send(id, reply.dump() + "\n");
}

std::optional<json> ControlRequest(const std::string& path, const json& request,
                                   int timeout_ms, std::string* err) {
  sockaddr_un a{};
  if (path.size() >= sizeof a.sun_path) {
    *err = "socket path too long: " + path;
    return std::nullopt;
  }
  a.sun_family = AF_UNIX;
  std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    *err = "cannot connect to " + path + " (" +
           std::strerror(errno) +  // NOLINT(concurrency-mt-unsafe)
           "); is lgrtkd running?";
    if (fd >= 0) {
      close(fd);
    }
    return std::nullopt;
  }
  const std::string out = request.dump() + "\n";
  if (send(fd, out.data(), out.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(out.size())) {
    *err = "lgrtkd closed the control connection";
    close(fd);
    return std::nullopt;
  }
  std::string in;
  const auto end =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (in.find('\n') == std::string::npos) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          end - std::chrono::steady_clock::now())
                          .count();
    pollfd p{fd, POLLIN, 0};
    if (left <= 0 || poll(&p, 1, static_cast<int>(left)) <= 0) {
      *err =
          "no answer from lgrtkd within " + std::to_string(timeout_ms) + " ms";
      close(fd);
      return std::nullopt;
    }
    char buf[4096];
    const ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) {
      *err = "lgrtkd closed the control connection";
      close(fd);
      return std::nullopt;
    }
    in.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  json reply = json::parse(in.substr(0, in.find('\n')), nullptr, false);
  if (!reply.is_object()) {
    *err = "lgrtkd sent an unreadable reply";
    return std::nullopt;
  }
  return reply;
}

}  // namespace lgrtk::daemon
