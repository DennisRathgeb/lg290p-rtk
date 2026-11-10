// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// lgrtkd: serves one Quectel LG290P system-wide (gpsd JSON, raw tap, chrony,
// control socket). Runs in the foreground; systemd supervises it.
#include <pthread.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "daemon/config.h"
#include "daemon/daemon.h"
#include "lgrtk/version.h"

namespace {

constexpr int kRuntime = 1;
constexpr int kUsage = 2;

const char kHelp[] = R"(lgrtkd - Quectel LG290P RTK daemon (gpsd protocol)

usage: lgrtkd [--config PATH] [--check-config]

  --config PATH    configuration file (default /etc/lgrtk/lgrtkd.toml;
                   a missing default file means built-in defaults)
  --check-config   parse the configuration, print the result and exit
  --version        print the version

exit codes: 0 stopped by SIGTERM/SIGINT, 1 runtime error at start,
            2 bad configuration or usage
)";

}  // namespace

int main(int argc, char** argv) {
  std::string path = "/etc/lgrtk/lgrtkd.toml";
  bool explicit_path = false;
  bool check = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      path = argv[++i];
      explicit_path = true;
    } else if (a == "--check-config") {
      check = true;
    } else if (a == "--version") {
      std::printf("lgrtkd %s\n", lgrtk::kVersion);
      return 0;
    } else if (a == "--help" || a == "-h") {
      std::fputs(kHelp, stdout);
      return 0;
    } else {
      std::fprintf(stderr, "lgrtkd: unknown argument '%s'\n%s", a.c_str(),
                   kHelp);
      return kUsage;
    }
  }

  std::string err;
  std::optional<lgrtk::daemon::DaemonConfig> cfg;
  if (!explicit_path && access(path.c_str(), F_OK) != 0) {
    cfg = lgrtk::daemon::ParseConfig("", &err);
  } else {
    cfg = lgrtk::daemon::LoadConfig(path, &err);
  }
  if (!cfg) {
    std::fprintf(stderr, "lgrtkd: %s\n", err.c_str());
    return kUsage;
  }
  if (check) {
    std::printf(
        "configuration ok\nreceiver %s @ %d (apply_config %s)\ngpsd %s\n"
        "raw %s\nchrony %s\ncontrol %s\nntrip %s\n",
        cfg->receiver.port.c_str(), cfg->receiver.baud,
        cfg->receiver.apply_config ? "true" : "false", cfg->gpsd_listen.c_str(),
        cfg->raw_listen.empty() ? "off" : cfg->raw_listen.c_str(),
        cfg->chrony_sock.empty() ? "off" : cfg->chrony_sock.c_str(),
        cfg->control_sock.empty() ? "off" : cfg->control_sock.c_str(),
        cfg->ntrip.enabled
            ? (cfg->ntrip.host + "/" + cfg->ntrip.mountpoint).c_str()
            : "off");
    return 0;
  }

  // Block the shutdown signals in every thread (the receiver and NTRIP
  // threads inherit this mask); the event loop reads them from a signalfd.
  const sigset_t set = lgrtk::daemon::ShutdownSignals();
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
  signal(SIGPIPE, SIG_IGN);

  lgrtk::daemon::Daemon daemon(std::move(*cfg));
  if (!daemon.Start(&err)) {
    std::fprintf(stderr, "lgrtkd: %s\n", err.c_str());
    return kRuntime;
  }
  daemon.Run(/*handle_signals=*/true);
  daemon.Stop();
  std::fprintf(stderr, "lgrtkd: stopped\n");
  return 0;
}
