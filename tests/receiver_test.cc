// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/receiver.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "sim/sim_lg290p.h"
#include "test_util.h"

namespace lgrtk {
namespace {

using namespace std::chrono_literals;

// Records status events and fixes; waits for conditions.
struct Observer {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<StatusEvent> events;
  std::vector<Fix> fixes;

  void Attach(Lg290pReceiver& r) {
    r.OnStatus([this](const StatusEvent& e) {
      std::lock_guard lk(mu);
      events.push_back(e);
      cv.notify_all();
    });
    r.OnFix([this](const Fix& f) {
      std::lock_guard lk(mu);
      fixes.push_back(f);
      cv.notify_all();
    });
  }
  bool WaitState(Lg290pReceiver& r, State s, std::chrono::milliseconds t) {
    std::unique_lock lk(mu);
    return cv.wait_for(lk, t, [&] { return r.Status().state == s; });
  }
  bool WaitFixes(size_t n, std::chrono::milliseconds t) {
    std::unique_lock lk(mu);
    return cv.wait_for(lk, t, [&] { return fixes.size() >= n; });
  }
  size_t FixCount() {
    std::lock_guard lk(mu);
    return fixes.size();
  }
};

ReceiverOptions Opts(const test::SimLg290p& sim) {
  ReceiverOptions o;
  o.port = sim.Path();
  o.command_timeout_ms = 400;
  o.backoff_min_ms = 50;
  o.backoff_max_ms = 400;
  o.stale_after_ms = 400;
  o.config = lg290p::DefaultRoverConfig();
  return o;
}

TEST(ReceiverTest, StreamsFixes) {
  test::SimLg290p sim;
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 5s));
  ASSERT_TRUE(obs.WaitFixes(5, 2s));
  const ReceiverStatus st = rx.Status();
  EXPECT_EQ(st.firmware, "LG290P03AANR01A06S");
  EXPECT_EQ(st.baud, 460800);
  const auto latest = rx.LatestFix();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->type, FixType::kSingle);
  EXPECT_EQ(latest->sigma_source,
            SigmaSource::kGst);  // GST enabled by the default config
  EXPECT_LT(st.last_fix_age_ns, 500'000'000);
  rx.Stop();
  EXPECT_EQ(rx.Status().state, State::kStopped);
}

TEST(ReceiverTest, PassesThroughEveryStateInOrder) {
  test::SimLg290p sim;
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 5s));
  rx.Stop();
  std::vector<State> seq;
  for (const auto& e : obs.events) {
    seq.push_back(e.to);
  }
  // The default config changes the fix-independent settings only: save, no
  // reset.
  const std::vector<State> want = {State::kOpening, State::kIdentifying,
                                   State::kConfiguring, State::kStreaming,
                                   State::kStopped};
  EXPECT_EQ(seq, want);
}

TEST(ReceiverTest, AppliesOnlyDifferences) {
  test::SimLg290p sim;
  {
    Observer obs;
    Lg290pReceiver rx(Opts(sim));
    obs.Attach(rx);
    ASSERT_TRUE(rx.Start());
    ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 5s));
  }
  EXPECT_FALSE(sim.Writes().empty());
  sim.ClearWrites();
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 5s));
  EXPECT_TRUE(sim.Writes().empty());  // nothing written, no NVM wear
}

TEST(ReceiverTest, ResetRequiringChangeResetsOnce) {
  test::SimLg290p sim;
  auto o = Opts(sim);
  o.config.fix_interval_ms = 200;
  Observer obs;
  Lg290pReceiver rx(o);
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 8s));
  EXPECT_EQ(sim.Resets(), 1);
}

TEST(ReceiverTest, DetectsBaud) {
  test::SimLg290p sim(115200);
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 10s));
  EXPECT_EQ(rx.Status().baud, 115200);
}

TEST(ReceiverTest, StaleWhenSilentThenRecovers) {
  test::SimLg290p sim;
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 5s));
  sim.SetSilent(true);
  ASSERT_TRUE(obs.WaitState(rx, State::kStale, 2s));
  sim.SetSilent(false);
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 2s));
}

TEST(ReceiverTest, UnplugReplugRecovers) {
  test::SimLg290p sim;
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 5s));
  ASSERT_TRUE(obs.WaitFixes(2, 2s));
  sim.Unplug();
  ASSERT_TRUE(obs.WaitState(rx, State::kReconnecting, 1s));
  std::this_thread::sleep_for(300ms);
  EXPECT_GT(rx.Status().last_fix_age_ns,
            250'000'000);  // the old fix ages, it is not fresh
  const size_t before = obs.FixCount();
  sim.Replug();
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 3s));
  ASSERT_TRUE(obs.WaitFixes(before + 2, 2s));
  EXPECT_GE(rx.Status().reconnects, 1u);
}

TEST(ReceiverTest, NakLeadsToFault) {
  test::SimLg290p sim;
  sim.Nak("PQTMCFGMSGRATE");
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kFault, 8s));
  const std::string err = rx.Status().last_error;
  EXPECT_NE(err.find("PQTMCFGMSGRATE"), std::string::npos) << err;
  EXPECT_NE(err.find("error 1"), std::string::npos) << err;
}

TEST(ReceiverTest, InjectReachesDevice) {
  test::SimLg290p sim;
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 5s));
  std::vector<uint8_t> payload(100, 0x42);
  payload[0] = 0x43;  // message 1077-ish, content irrelevant
  const auto frame = test::Rtcm(payload);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(rx.Inject(frame));
  }
  for (int i = 0; i < 50 && sim.RtcmReceived().size() < 5 * frame.size(); ++i) {
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_EQ(sim.RtcmReceived().size(), 5 * frame.size());
}

TEST(ReceiverTest, SendCommandWhileStreaming) {
  test::SimLg290p sim;
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 5s));
  auto fut = rx.Send(lg290p::GetFixRate());
  ASSERT_EQ(fut.wait_for(2s), std::future_status::ready);
  const auto reply = fut.get();
  ASSERT_TRUE(reply);
  EXPECT_TRUE(reply->ok);
  EXPECT_EQ(reply->values.at(0), "100");
}

TEST(ReceiverTest, StopJoinsFast) {
  test::SimLg290p sim;
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitState(rx, State::kStreaming, 5s));
  const auto t0 = std::chrono::steady_clock::now();
  rx.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
}

TEST(ReceiverTest, MissingPortKeepsRetrying) {
  ReceiverOptions o;
  o.port = "/dev/does-not-exist-lgrtk";
  o.backoff_min_ms = 20;
  o.backoff_max_ms = 40;
  Observer obs;
  Lg290pReceiver rx(o);
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  std::this_thread::sleep_for(200ms);
  const auto st = rx.Status();
  EXPECT_EQ(st.state, State::kReconnecting);
  EXPECT_NE(st.last_error.find("does-not-exist"), std::string::npos);
}

TEST(ReceiverTest, StaleBytesAtOpenAreDiscarded) {
  // Bench 2026-11-10: the CH343 delivered an epoch from an hour earlier on
  // open.
  test::SimLg290p sim;
  sim.SetBurstOnOpen(true);
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitFixes(5, 5s));
  std::lock_guard lk(obs.mu);
  for (const Fix& f : obs.fixes) {
    EXPECT_GE(f.utc_s, 36000.0) << "old epoch published as fresh";
  }
}

TEST(ReceiverTest, RawRecorderWritesStream) {
  test::SimLg290p sim;
  const auto path =
      std::filesystem::temp_directory_path() / "lgrtk-raw-test.bin";
  std::string err;
  auto rec = RawRecorder::Open(path.string(), err);
  ASSERT_TRUE(rec) << err;
  Observer obs;
  Lg290pReceiver rx(Opts(sim));
  obs.Attach(rx);
  rx.OnRaw([&](std::span<const uint8_t> b) { rec->Write(b); });
  ASSERT_TRUE(rx.Start());
  ASSERT_TRUE(obs.WaitFixes(3, 5s));
  rx.Stop();
  rec.reset();
  std::ifstream in(path);
  const std::string content((std::istreambuf_iterator<char>(in)), {});
  EXPECT_NE(content.find("$GNGGA,"), std::string::npos);
  std::filesystem::remove(path);
}

}  // namespace
}  // namespace lgrtk
