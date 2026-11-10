// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "apps/lgrtk/session.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <future>
#include <iostream>
#include <thread>
#include <utility>

#include "apps/lgrtk/args.h"

namespace lgrtk::cli {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPollInterval = std::chrono::milliseconds(20);
constexpr auto kReplyTimeout = std::chrono::seconds(5);

std::atomic<bool> g_interrupted{false};

void OnSignal(int /*signal*/) { g_interrupted = true; }

// Waits until `rx` streams; false on a fault, a timeout or Ctrl-C.
bool WaitStreaming(Lg290pReceiver& rx, int timeout_s) {
  const auto end = Clock::now() + std::chrono::seconds(timeout_s);
  while (Clock::now() < end && !Interrupted()) {
    const auto st = rx.Status();
    if (st.state == State::kStreaming) {
      return true;
    }
    if (st.state == State::kFault) {
      std::cerr << "lgrtk: receiver fault: " << st.last_error << "\n";
      return false;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  const auto st = rx.Status();
  std::cerr << "lgrtk: receiver not ready (state " << ToString(st.state) << ")"
            << (st.last_error.empty() ? "" : ": " + st.last_error) << "\n";
  return false;
}

}  // namespace

void InstallSignalHandlers() {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
}

bool Interrupted() { return g_interrupted; }

void WaitInterruptible(std::optional<int> seconds,
                       const std::function<bool()>& done) {
  const auto t0 = Clock::now();
  while (!Interrupted() && !(done && done()) &&
         (!seconds || Clock::now() - t0 < std::chrono::seconds(*seconds))) {
    std::this_thread::sleep_for(kPollInterval);
  }
}

bool StartReceiver(Lg290pReceiver& rx, int timeout_s) {
  if (!rx.Start()) {
    return false;
  }
  if (WaitStreaming(rx, timeout_s)) {
    return true;
  }
  rx.Stop();
  return false;
}

std::optional<nmea::OwnedReply> Ask(Lg290pReceiver& rx, lg290p::Command cmd) {
  auto fut = rx.Send(std::move(cmd));
  if (fut.wait_for(kReplyTimeout) != std::future_status::ready) {
    return std::nullopt;
  }
  return fut.get();
}

int RunCommands(Lg290pReceiver& rx, const std::vector<lg290p::Command>& cmds) {
  for (const auto& c : cmds) {
    if (c.reply_to.empty()) {
      (void)Ask(rx, c);
      std::printf("sent %s", c.text.c_str());
      continue;
    }
    const auto r = Ask(rx, c);
    if (!r) {
      std::cerr << "lgrtk: no reply to " << c.text;
      return kRuntime;
    }
    if (!r->ok) {
      std::cerr << "lgrtk: receiver refused " << c.text << "  (error "
                << r->error_code
                << ": 1 invalid parameters, 2 failed, 3 unsupported)\n";
      return kRuntime;
    }
    std::printf("%s", r->command.c_str());
    for (const auto& v : r->values) {
      std::printf(" %s", v.c_str());
    }
    std::printf("\n");
  }
  return kOk;
}

}  // namespace lgrtk::cli
