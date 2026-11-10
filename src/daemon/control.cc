// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "daemon/control.h"

#include <unistd.h>

#include <utility>

namespace lgrtk::daemon {
namespace {

using nlohmann::json;

json Fail(const std::string& msg) {
  return json{{kFieldOk, false}, {kFieldError, msg}};
}

// Merges the handler's fields under "ok"; an "error" field marks failure.
json Reply(json fields) {
  json r{{kFieldOk, !fields.contains(kFieldError)}};
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
  if (!req.is_object() || !req.contains(kFieldCmd) ||
      !req[kFieldCmd].is_string()) {
    reply = Fail("expected {\"cmd\": ...}");
  } else {
    const std::string cmd = req[kFieldCmd].get<std::string>();
    if (cmd == kCmdStatus && h_.status) {
      reply = Reply(h_.status());
    } else if (cmd == kCmdReset && h_.reset) {
      const std::string kind =
          req.contains(kFieldKind) && req[kFieldKind].is_string()
              ? req[kFieldKind].get<std::string>()
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
    } else if (cmd == kCmdApplyDefault && h_.apply_default) {
      reply = Reply(h_.apply_default());
    } else {
      reply = Fail("unknown command '" + cmd + "'");
    }
  }
  clients_.Send(id, reply.dump() + "\n");
}

}  // namespace lgrtk::daemon
