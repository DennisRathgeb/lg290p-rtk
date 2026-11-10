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
int CmdProbe(Args& a, const ReceiverOptions& options);   ///< `probe`
int CmdWatch(Args& a, const ReceiverOptions& options);   ///< `watch`
int CmdStatus(Args& a, const ReceiverOptions& options);  ///< `status`

// cmd_config.cc: change the receiver.
int CmdConfig(
    Args& a,
    const ReceiverOptions& options);  ///< `config get|set|apply-default`
int CmdReset(Args& a, const ReceiverOptions& options);  ///< `reset`
int CmdBase(Args& a, const ReceiverOptions& options);   ///< `base`

// cmd_ntrip.cc
int CmdNtrip(Args& a,
             const ReceiverOptions& options);  ///< `ntrip sourcetable|run`

// cmd_project.cc
int CmdProject(Args& a,
               const ReceiverOptions& options);  ///< `project` (options unused)

#ifdef LGRTK_HAVE_CTL
// cmd_ctl.cc
int CmdCtl(Args& a,
           const ReceiverOptions& options);  ///< `ctl` (options unused)
#endif

}  // namespace lgrtk::cli

#endif  // LGRTK_APPS_LGRTK_COMMANDS_H_
