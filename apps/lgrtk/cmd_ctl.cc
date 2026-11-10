// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// lgrtk ctl: one request to a running lgrtkd's control socket.
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "apps/lgrtk/args.h"
#include "apps/lgrtk/commands.h"
#include "daemon/control_client.h"

namespace lgrtk::cli {
namespace {

constexpr int kCtlTimeoutMs = 5000;

}  // namespace

int CmdCtl(Args& a, ReceiverOptions /*o*/) {
  namespace d = lgrtk::daemon;
  const std::string sock =
      a.Option("--socket").value_or(d::kDefaultControlSocket);
  const std::string sub = a.Next();
  nlohmann::json req;
  if (sub == d::kCmdStatus || sub == d::kCmdApplyDefault) {
    req = {{d::kFieldCmd, sub}};
  } else if (sub == d::kCmdReset) {
    req = {{d::kFieldCmd, d::kCmdReset}, {d::kFieldKind, a.Next()}};
  } else {
    return Usage("ctl: expected status, reset KIND or apply-default");
  }
  if (a.Has()) {
    return Usage("ctl: unexpected argument " + a.Peek());
  }
  std::string err;
  const auto reply = d::ControlRequest(sock, req, kCtlTimeoutMs, &err);
  if (!reply) {
    std::cerr << "lgrtk: " << err << "\n";
    return kRuntime;
  }
  if (!reply->value(d::kFieldOk, false)) {
    std::cerr << "lgrtk: "
              << reply->value(d::kFieldError, std::string("failed")) << "\n";
    return kRuntime;
  }
  for (const auto& [key, value] : reply->items()) {
    if (key == d::kFieldOk) {
      continue;
    }
    std::cout << key << ": "
              << (value.is_string() ? value.get<std::string>() : value.dump())
              << "\n";
  }
  return kOk;
}

}  // namespace lgrtk::cli
