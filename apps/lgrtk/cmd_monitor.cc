// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
// lgrtk probe, watch, status: read-only, the receiver's configuration is
// never changed.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include "apps/lgrtk/args.h"
#include "apps/lgrtk/commands.h"
#include "apps/lgrtk/format.h"
#include "apps/lgrtk/session.h"
#include "lgrtk/receiver.h"

namespace lgrtk::cli {

int CmdProbe(Args& a, const ReceiverOptions& options) {
  ReceiverOptions o = options;
  int seconds = 60;
  if (auto s = a.Option("--seconds"); s && !ToInt(*s, seconds)) {
    return Usage("--seconds needs a number");
  }
  const auto raw = a.Option("--raw");
  if (a.Has()) {
    return Usage("probe: unexpected argument " + a.Peek());
  }
  // Everything the callbacks capture is declared before the receiver, so it
  // outlives the I/O thread on every return path.
  std::optional<RawRecorder> rec;
  if (raw) {
    std::string err;
    rec = RawRecorder::Open(*raw, err);
    if (!rec) {
      std::cerr << "lgrtk: " << err << "\n";
      return kRuntime;
    }
  }
  std::mutex mu;
  ProbeStats stats;
  o.apply_config = false;
  Lg290pReceiver rx(o);
  if (rec) {
    rx.OnRaw([&](std::span<const uint8_t> b) { rec->Write(b); });
  }
  rx.OnNmea([&](std::string_view line) {
    const std::scoped_lock lk(mu);
    stats.Add(line);
  });
  if (!StartReceiver(rx)) {
    return kRuntime;
  }
  {
    // Count only what arrives after the start-up.
    const std::scoped_lock lk(mu);
    stats = ProbeStats{};
  }
  const auto t0 = std::chrono::steady_clock::now();
  WaitInterruptible(seconds);
  const double dt =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  const auto st = rx.Status();
  rx.Stop();
  const std::scoped_lock lk(mu);
  PrintProbeReport(st, stats, dt);
  if (rec && raw) {
    std::printf("raw: %llu bytes written to %s\n",
                static_cast<unsigned long long>(rec->Bytes()), raw->c_str());
  }
  return kOk;
}

int CmdWatch(Args& a, const ReceiverOptions& options) {
  ReceiverOptions o = options;
  int count = 0;
  if (auto s = a.Option("--count"); s && !ToInt(*s, count)) {
    return Usage("--count needs a number");
  }
  if (a.Has()) {
    return Usage("watch: unexpected argument " + a.Peek());
  }
  std::atomic<int> seen{0};
  o.apply_config = false;
  Lg290pReceiver rx(o);
  rx.OnFix([&](const Fix& f) {
    if (count > 0 && seen >= count) {
      return;
    }
    std::printf("%s\n", FormatFix(f).c_str());
    std::fflush(stdout);
    ++seen;
  });
  rx.OnStatus([](const StatusEvent& e) { PrintEvent(stderr, "", e); });
  if (!rx.Start()) {
    return kRuntime;
  }
  WaitInterruptible(std::nullopt, [&] { return count > 0 && seen >= count; });
  rx.Stop();
  return kOk;
}

int CmdStatus(Args& a, const ReceiverOptions& options) {
  ReceiverOptions o = options;
  int seconds = 5;
  if (auto s = a.Option("--seconds"); s && !ToInt(*s, seconds)) {
    return Usage("--seconds needs a number");
  }
  o.apply_config = false;
  Lg290pReceiver rx(o);
  rx.OnStatus([](const StatusEvent& e) { PrintEvent(stdout, "", e); });
  if (!rx.Start()) {
    return kRuntime;
  }
  WaitInterruptible(seconds);
  const auto st = rx.Status();
  rx.Stop();
  PrintStatusReport(st);
  return st.state == State::kFault ? kRuntime : kOk;
}

}  // namespace lgrtk::cli
