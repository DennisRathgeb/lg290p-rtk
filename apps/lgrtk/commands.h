// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// lgrtk CLI: the commands. Each takes the remaining arguments and the
/// receiver options from --port/--baud, and returns an exit code.
#ifndef LGRTK_APPS_LGRTK_COMMANDS_H_
#define LGRTK_APPS_LGRTK_COMMANDS_H_

#include "apps/lgrtk/args.h"
#include "lgrtk/receiver.h"

namespace lgrtk::cli {

// cmd_monitor.cc: read-only.
int CmdProbe(Args& a, ReceiverOptions o);   ///< `probe`
int CmdWatch(Args& a, ReceiverOptions o);   ///< `watch`
int CmdStatus(Args& a, ReceiverOptions o);  ///< `status`

// cmd_config.cc: change the receiver.
int CmdConfig(Args& a, ReceiverOptions o);  ///< `config get|set|apply-default`
int CmdReset(Args& a, ReceiverOptions o);   ///< `reset`
int CmdBase(Args& a, ReceiverOptions o);    ///< `base`

// cmd_ntrip.cc
int CmdNtrip(Args& a, ReceiverOptions o);  ///< `ntrip sourcetable|run`

// cmd_project.cc
int CmdProject(Args& a, ReceiverOptions o);  ///< `project` (o unused)

#ifdef LGRTK_HAVE_CTL
// cmd_ctl.cc
int CmdCtl(Args& a, ReceiverOptions o);  ///< `ctl` (o unused)
#endif

}  // namespace lgrtk::cli

#endif  // LGRTK_APPS_LGRTK_COMMANDS_H_
