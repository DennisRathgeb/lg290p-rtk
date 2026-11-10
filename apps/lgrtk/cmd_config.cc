// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// lgrtk config, reset, base: commands that change the receiver.
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "apps/lgrtk/args.h"
#include "apps/lgrtk/commands.h"
#include "apps/lgrtk/session.h"
#include "lgrtk/lg290p.h"
#include "lgrtk/receiver.h"

namespace lgrtk::cli {
namespace {

namespace lg = lgrtk::lg290p;

// Opens the receiver without configuring it and sends `cmds`.
int Run(const ReceiverOptions& base, const std::vector<lg::Command>& cmds) {
  ReceiverOptions o = base;
  o.apply_config = false;
  Lg290pReceiver rx(o);
  if (!StartReceiver(rx)) {
    return kRuntime;
  }
  return RunCommands(rx, cmds);
}

// The Get command for `key` (`vals` holds MSG [VER] for msg-rate); nullopt
// for an unknown key or bad values.
std::optional<lg::Command> GetCommand(const std::string& key,
                                      const std::vector<std::string>& vals) {
  if (key == "fix-rate") {
    return lg::GetFixRate();
  }
  if (key == "msg-rate" && !vals.empty()) {
    int version = 0;
    if (vals.size() > 1 && !ToInt(vals[1], version)) {
      return std::nullopt;
    }
    return lg::GetMessageRate(vals[0], version);
  }
  if (key == "rtk") {
    return lg::GetRtk();
  }
  if (key == "elevation-mask") {
    return lg::GetElevationMask();
  }
  if (key == "cnr-mask") {
    return lg::GetCnrMask();
  }
  if (key == "decimals") {
    return lg::GetNmeaDecimals();
  }
  if (key == "constellations") {
    return lg::GetConstellations();
  }
  if (key == "mode") {
    return lg::GetReceiverMode();
  }
  if (key == "pps") {
    return lg::GetPps();
  }
  return std::nullopt;
}

// The Set command for `key`; `bad_values` tells a bad key or value count
// (usage) from a value the library rejects as out of range (nullopt with
// `bad_values` false).
std::optional<lg::Command> SetCommand(const std::string& key,
                                      const std::vector<std::string>& vals,
                                      bool& bad_values) {
  std::vector<int> n(vals.size());
  bool all_int = true;
  for (size_t i = 0; i < vals.size(); ++i) {
    all_int = ToInt(vals[i], n[i]) && all_int;
  }
  double d = 0;
  int version = 0;
  bad_values = false;
  if (key == "fix-rate" && vals.size() == 1 && all_int) {
    return lg::SetFixRate(n[0]);
  }
  if (key == "msg-rate" && (vals.size() == 2 || vals.size() == 3) &&
      ToInt(vals[1], n[1]) && (vals.size() == 2 || ToInt(vals[2], version))) {
    return lg::SetMessageRate(vals[0], n[1], version);
  }
  if (key == "rtk" && vals.size() == 3 && all_int) {
    return lg::SetRtk({n[0], n[1], n[2]});
  }
  if (key == "elevation-mask" && vals.size() == 1 && ToDouble(vals[0], d)) {
    return lg::SetElevationMask(d);
  }
  if (key == "cnr-mask" && vals.size() == 1 && ToDouble(vals[0], d)) {
    return lg::SetCnrMask(d);
  }
  if (key == "decimals" && vals.size() == 6 && all_int) {
    return lg::SetNmeaDecimals({n[0], n[1], n[2], n[3], n[4], n[5]});
  }
  if (key == "constellations" && vals.size() == 6 && all_int) {
    return lg::SetConstellations(
        {n[0] != 0, n[1] != 0, n[2] != 0, n[3] != 0, n[4] != 0, n[5] != 0});
  }
  bad_values = true;
  return std::nullopt;
}

int ApplyDefault(const ReceiverOptions& base) {
  ReceiverOptions o = base;
  o.config = lg::DefaultRoverConfig();
  o.apply_config = true;
  Lg290pReceiver rx(o);
  if (!StartReceiver(rx, 30)) {
    return kRuntime;
  }
  const auto st = rx.Status();
  std::printf("default rover configuration applied (%s)\n",
              st.last_error.empty() ? "ok" : st.last_error.c_str());
  return kOk;
}

}  // namespace

int CmdConfig(Args& a, const ReceiverOptions& options) {
  const std::string op = a.Next();
  const std::string key = a.Next();
  std::vector<std::string> vals;
  while (a.Has()) {
    vals.push_back(a.Next());
  }
  if (op == "get") {
    const auto c = GetCommand(key, vals);
    if (!c && key == "msg-rate" && vals.size() > 1) {
      return Usage("config get msg-rate: VER needs a number");
    }
    if (!c) {
      return Usage("config get: unknown key '" + key + "'");
    }
    return Run(options, {*c});
  }
  if (op == "set") {
    bool bad_values = false;
    const auto c = SetCommand(key, vals, bad_values);
    if (bad_values) {
      return Usage("config set: bad key or values for '" + key + "'");
    }
    if (!c) {
      return Usage("config set " + key + ": value out of range");
    }
    if (c->needs_reset) {
      std::fprintf(stderr, "note: %s takes effect after `lgrtk reset soft`\n",
                   key.c_str());
    }
    return Run(options, {*c, lg::SaveParams()});
  }
  if (op == "apply-default") {
    return ApplyDefault(options);
  }
  return Usage("config: expected get, set or apply-default");
}

int CmdReset(Args& a, const ReceiverOptions& options) {
  const bool yes = a.Flag("--yes");
  const std::string kind = a.Next();
  std::vector<lg::Command> cmds;
  if (auto c = lg::RestartByName(kind)) {
    cmds = {*c};
  } else if (kind == "factory") {
    if (!yes) {
      return Usage(
          "reset factory erases the receiver's saved configuration: add --yes");
    }
    cmds = {lg::RestoreDefaults(), lg::SoftwareReset()};
  } else {
    return Usage("reset: expected hot, warm, cold, soft or factory");
  }
  return Run(options, cmds);
}

int CmdBase(Args& a, const ReceiverOptions& options) {
  const std::string mode = a.Next();
  std::vector<lg::Command> cmds;
  if (mode == "survey-in") {
    int s = 0;
    double acc = 0;
    if (!ToInt(a.Next(), s) || !ToDouble(a.Next(), acc)) {
      return Usage("base survey-in SECONDS ACC_M");
    }
    auto c = lg::SurveyIn(s, acc);
    if (!c) {
      return Usage("base survey-in: values out of range");
    }
    cmds = {lg::SetReceiverMode(lg::Mode::kBase), *c};
  } else if (mode == "fixed") {
    double x = 0;
    double y = 0;
    double z = 0;
    if (!ToDouble(a.Next(), x) || !ToDouble(a.Next(), y) ||
        !ToDouble(a.Next(), z)) {
      return Usage("base fixed X Y Z");
    }
    cmds = {lg::SetReceiverMode(lg::Mode::kBase), lg::FixedBase(x, y, z)};
  } else if (mode == "rover") {
    cmds = {lg::DisableSurvey(), lg::SetReceiverMode(lg::Mode::kRover)};
  } else {
    return Usage("base: expected survey-in, fixed or rover");
  }
  cmds.push_back(lg::SaveParams());
  cmds.push_back(lg::SoftwareReset());
  const int rc = Run(options, cmds);
  if (rc == kOk && mode != "rover") {
    std::printf(
        "base mode set; watch progress with `lgrtk status` (PQTMSVINSTATUS)\n");
  }
  return rc;
}

}  // namespace lgrtk::cli
