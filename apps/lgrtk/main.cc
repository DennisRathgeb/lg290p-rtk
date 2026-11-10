// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// lgrtk: command-line tool for the Quectel LG290P.
//
// Talks to the receiver directly over its serial port (one owner at a time).
// Read-only unless a command says otherwise: probe, watch and status never
// change the receiver's configuration.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "apps/lgrtk/args.h"
#include "apps/lgrtk/commands.h"
#include "apps/lgrtk/session.h"
#include "lgrtk/receiver.h"
#include "lgrtk/version.h"

namespace lgrtk::cli {
namespace {

const char kHelp[] = R"(lgrtk - Quectel LG290P RTK receiver tool

usage: lgrtk [--port PATH] [--baud N] <command> [args]

  --port PATH   serial device (default /dev/ttyACM0, or $LGRTK_PORT)
  --baud N      line speed tried first (default 460800; others are detected)

commands (read-only):
  probe [--seconds N] [--raw FILE]    what the receiver sends: rates, fix, firmware
  watch [--count N]                   one line per fix
  status [--seconds N]                state machine events and counters
  version                             tool version

commands that change the receiver:
  config get <key> [MSG]              key: fix-rate | msg-rate MSG | rtk | elevation-mask |
                                           cnr-mask | decimals | constellations | mode | pps
  config set <key> <values...>        fix-rate MS | msg-rate MSG N [VER] | rtk DIFF REL TIMEOUT |
                                      elevation-mask DEG | cnr-mask DBHZ | decimals U P A D S C |
                                      constellations GPS GLO GAL BDS QZSS NAVIC (0/1)
                                      (saved to flash; fix-rate/constellations need `reset soft`)
  config apply-default                10 Hz, GGA/RMC/GST/EPE, high-precision NMEA, RTK auto
  reset hot|warm|cold|soft            restart the receiver
  reset factory --yes                 factory defaults (then reset soft)
  base survey-in SECONDS ACC_M        become a base: survey-in
  base fixed X Y Z                    become a base at an ECEF position (metres)
  base rover                          back to rover mode

corrections and coordinates:
  ntrip sourcetable HOST[:PORT] [--v1]
  ntrip run --host H [--port-ntrip P] --mount M [--user U] [--password-file F] [--v1] [--no-gga]
                                      receiver + NTRIP corrections, one line per fix
  project --from CRS --to CRS LAT LON H   e.g. --from EPSG:4937 --to EPSG:2056+5729

talking to a running lgrtkd (which owns the port):
  ctl [--socket PATH] status          daemon and receiver state, counters
  ctl [--socket PATH] reset hot|warm|cold|soft
  ctl [--socket PATH] apply-default   same as `config apply-default`, through the daemon
                                      (default socket /run/lgrtkd/control.sock)

exit codes: 0 ok, 1 runtime error, 2 usage error
)";

int CmdVersion(Args& /*a*/, ReceiverOptions /*o*/) {
  std::printf("lgrtk %s\n", kVersion);
  return kOk;
}

struct Command {
  std::string_view name;
  int (*fn)(Args&, ReceiverOptions);
};

constexpr Command kCommands[] = {
    {"version", CmdVersion}, {"probe", CmdProbe},   {"watch", CmdWatch},
    {"status", CmdStatus},   {"config", CmdConfig}, {"reset", CmdReset},
    {"base", CmdBase},       {"ntrip", CmdNtrip},   {"project", CmdProject},
#ifdef LGRTK_HAVE_CTL
    {"ctl", CmdCtl},
#endif
};

int Main(int argc, char** argv) {
  InstallSignalHandlers();
  Args a(argc, argv);
  if (a.Empty() || a.Flag("--help") || a.Flag("-h")) {
    std::cout << kHelp;
    return a.Empty() ? kUsage : kOk;
  }
  ReceiverOptions o;
  if (const char* env = std::getenv("LGRTK_PORT")) {
    o.port = env;
  }
  if (auto p = a.Option("--port")) {
    o.port = *p;
  }
  if (auto b = a.Option("--baud"); b && !ToInt(*b, o.baud)) {
    return Usage("--baud needs a number");
  }
  const std::string cmd = a.Next();
  for (const auto& c : kCommands) {
    if (c.name == cmd) {
      return c.fn(a, o);
    }
  }
  return Usage("unknown command '" + cmd + "'");
}

}  // namespace
}  // namespace lgrtk::cli

int main(int argc, char** argv) { return lgrtk::cli::Main(argc, argv); }
