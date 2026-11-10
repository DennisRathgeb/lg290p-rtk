// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// lgrtk ntrip: a caster's sourcetable, or the receiver with NTRIP
// corrections.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "apps/lgrtk/args.h"
#include "apps/lgrtk/commands.h"
#include "apps/lgrtk/format.h"
#include "apps/lgrtk/session.h"
#include "internal/clock.h"
#include "internal/host_port.h"
#include "lgrtk/lg290p.h"
#include "lgrtk/ntrip.h"
#include "lgrtk/receiver.h"

namespace lgrtk::cli {
namespace {

constexpr int kDefaultCasterPort = 2101;

// Stops an NTRIP client and its receiver when it goes out of scope: the
// receiver's OnFix uses the client and the client injects into the receiver,
// so neither thread may outlive the other object.
class StopBoth {
 public:
  StopBoth(Lg290pReceiver& rx, NtripClient& nc) : rx_(rx), nc_(nc) {}
  StopBoth(const StopBoth&) = delete;
  StopBoth& operator=(const StopBoth&) = delete;
  StopBoth(StopBoth&&) = delete;
  StopBoth& operator=(StopBoth&&) = delete;
  ~StopBoth() {
    nc_.Stop();
    rx_.Stop();
  }

 private:
  Lg290pReceiver& rx_;
  NtripClient& nc_;
};

int Sourcetable(Args& a, bool v1) {
  const std::string hp = a.Next();
  if (hp.empty()) {
    return Usage("ntrip sourcetable HOST[:PORT]");
  }
  std::string host = hp;
  int port = kDefaultCasterPort;
  if (hp.find(':') != std::string::npos &&
      !internal::ParseHostPort(hp, &host, &port)) {
    return Usage("bad port in " + hp);
  }
  std::string err;
  const auto t = NtripClient::FetchSourcetable(host, port, v1 ? 1 : 2, err);
  if (!t) {
    std::cerr << "lgrtk: " << err << "\n";
    return kRuntime;
  }
  std::cout << *t;
  return kOk;
}

}  // namespace

int CmdNtrip(Args& a, ReceiverOptions o) {
  const std::string op = a.Next();
  const bool v1 = a.Flag("--v1");
  if (op == "sourcetable") {
    return Sourcetable(a, v1);
  }
  if (op != "run") {
    return Usage("ntrip: expected sourcetable or run");
  }
  NtripOptions no;
  no.version = v1 ? 1 : 2;
  no.send_gga = !a.Flag("--no-gga");
  if (auto h = a.Option("--host")) {
    no.host = *h;
  }
  if (auto p = a.Option("--port-ntrip"); p && !ToInt(*p, no.port)) {
    return Usage("--port-ntrip needs a number");
  }
  if (auto m = a.Option("--mount")) {
    no.mountpoint = *m;
  }
  if (auto u = a.Option("--user")) {
    no.user = *u;
  }
  if (auto f = a.Option("--password-file")) {
    std::string err;
    auto pw = internal::ReadFirstLine(*f, &err);
    if (!pw) {
      std::cerr << "lgrtk: cannot read password file " << *f << "\n";
      return kRuntime;
    }
    no.password = std::move(*pw);
  } else if (const char* env = std::getenv("LGRTK_NTRIP_PASSWORD")) {
    no.password = env;
  }
  if (no.host.empty() || no.mountpoint.empty()) {
    return Usage("ntrip run needs --host and --mount");
  }
  o.config = lg290p::DefaultRoverConfig();
  Lg290pReceiver rx(o);
  NtripClient nc(no, rx);
  rx.OnFix([&](const Fix& f) {
    nc.SetPosition(f);
    const auto ns = nc.Status();
    std::printf(
        "%s  ntrip %s %.1f s\n", FormatFix(f).c_str(), ToString(ns.state),
        ns.last_data_age_ns < 0 ? -1.0
                                : static_cast<double>(ns.last_data_age_ns) /
                                      static_cast<double>(internal::kNsPerS));
    std::fflush(stdout);
  });
  rx.OnStatus([](const StatusEvent& e) { PrintEvent(stderr, "receiver", e); });
  nc.OnStatus([](NtripState from, NtripState to, const std::string& why) {
    PrintTransition(stderr, "ntrip", ToString(from), ToString(to), why);
  });
  const StopBoth stop(rx, nc);
  if (!rx.Start() || !nc.Start()) {
    return kRuntime;
  }
  WaitInterruptible(std::nullopt);
  return kOk;
}

}  // namespace lgrtk::cli
