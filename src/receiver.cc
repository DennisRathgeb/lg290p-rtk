// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#include "lgrtk/receiver.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include "internal/clock.h"
namespace lgrtk {

namespace {

using internal::kNsPerMs;
using internal::MonotonicNs;
using internal::MsToNs;

constexpr int kIdentifyRounds = 3;
constexpr size_t kInjectBudget = 4096;  // bytes per loop iteration
constexpr int kPumpMs = 50;
// USB bridges (the CH343 on the Waveshare board) hand over bytes left in
// their FIFO from the previous session right after open; tcflush does not
// reach them. Everything read this long after open is discarded.
constexpr int64_t kOpenDrainNs = MsToNs(150);

bool PathExists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

}  // namespace

const char* ToString(State s) noexcept {
  switch (s) {
    case State::kDisconnected:
      return "disconnected";
    case State::kOpening:
      return "opening";
    case State::kIdentifying:
      return "identifying";
    case State::kConfiguring:
      return "configuring";
    case State::kStreaming:
      return "streaming";
    case State::kStale:
      return "stale";
    case State::kReconnecting:
      return "reconnecting";
    case State::kFault:
      return "fault";
    case State::kStopped:
      return "stopped";
  }
  return "?";
}

// ---------------------------------------------------------------- RawRecorder

std::optional<RawRecorder> RawRecorder::Open(const std::string& path,
                                             std::string& err) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    err = path + ": " + std::strerror(errno);
    return std::nullopt;
  }
  std::setvbuf(f, nullptr, _IOFBF, 1 << 16);
  return RawRecorder(f);
}

RawRecorder::RawRecorder(RawRecorder&& o) noexcept
    : f_(std::exchange(o.f_, nullptr)), bytes_(o.bytes_) {}

RawRecorder& RawRecorder::operator=(RawRecorder&& o) noexcept {
  if (this != &o) {
    if (f_ != nullptr) {
      std::fclose(f_);
    }
    f_ = std::exchange(o.f_, nullptr);
    bytes_ = o.bytes_;
  }
  return *this;
}

RawRecorder::~RawRecorder() {
  if (f_ != nullptr) {
    std::fclose(f_);
  }
}

void RawRecorder::Write(std::span<const uint8_t> bytes) noexcept {
  if (f_ == nullptr) {
    return;
  }
  bytes_ += std::fwrite(bytes.data(), 1, bytes.size(), f_);
}

// ------------------------------------------------------------- Lg290pReceiver

Lg290pReceiver::Lg290pReceiver(ReceiverOptions options)
    : opt_(std::move(options)), ring_(std::make_unique<uint8_t[]>(kRingSize)) {
  status_.port = opt_.port;
  status_.baud = opt_.baud;
}

Lg290pReceiver::~Lg290pReceiver() { Stop(); }

bool Lg290pReceiver::Start() {
  if (running_.exchange(true)) {
    return false;
  }
  stop_ = false;
  thread_ = std::thread([this] { Run(); });
  return true;
}

void Lg290pReceiver::Stop() {
  if (!running_) {
    return;
  }
  stop_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  running_ = false;
}

ReceiverStatus Lg290pReceiver::Status() const {
  std::lock_guard lk(mu_);
  ReceiverStatus s = status_;
  s.last_fix_age_ns = latest_ ? MonotonicNs() - latest_->t_arrival_ns : -1;
  return s;
}

std::optional<Fix> Lg290pReceiver::LatestFix() const {
  std::lock_guard lk(mu_);
  return latest_;
}

FixSnapshot Lg290pReceiver::Snapshot() const {
  std::lock_guard lk(mu_);
  FixSnapshot s;
  s.fix = latest_;
  s.age_ns = latest_ ? MonotonicNs() - latest_->t_arrival_ns : -1;
  s.state = status_.state;
  return s;
}

std::future<std::optional<nmea::OwnedReply>> Lg290pReceiver::Send(
    lg290p::Command cmd) {
  Pending p{std::move(cmd), {}, 0};
  auto fut = p.promise.get_future();
  if (!running_) {
    p.promise.set_value(std::nullopt);  // nobody would ever execute it
    return fut;
  }
  std::lock_guard lk(mu_);
  queue_.push_back(std::move(p));
  return fut;
}

bool Lg290pReceiver::Inject(std::span<const uint8_t> frames) {
  const size_t head = ring_head_.load(std::memory_order_relaxed);
  const size_t tail = ring_tail_.load(std::memory_order_acquire);
  if (frames.size() > kRingSize - (head - tail)) {
    std::lock_guard lk(mu_);
    status_.inject_dropped += frames.size();
    return false;
  }
  for (size_t i = 0; i < frames.size(); ++i) {
    ring_[(head + i) % kRingSize] = frames[i];
  }
  ring_head_.store(head + frames.size(), std::memory_order_release);
  return true;
}

void Lg290pReceiver::SetState(State s, std::string reason) {
  StatusEvent ev{};
  {
    std::lock_guard lk(mu_);
    if (status_.state == s) {
      if (s == State::kReconnecting) {
        status_.last_error = reason;
      }
      return;
    }
    ev.from = status_.state;
    status_.state = s;
    if (s == State::kReconnecting || s == State::kFault) {
      status_.last_error = reason;
    }
  }
  ev.to = s;
  ev.reason = std::move(reason);
  ev.t_ns = MonotonicNs();
  if (on_status_) {
    on_status_(ev);
  }
}

void Lg290pReceiver::Run() {
  backoff_ms_ = opt_.backoff_min_ms;
  enum class Phase { kConnect, kIdentify, kStream } phase = Phase::kConnect;
  {
    std::lock_guard lk(mu_);
    if (status_.state == State::kStopped) {
      status_.state = State::kDisconnected;
    }
  }

  while (!StopRequested()) {
    State current;
    {
      std::lock_guard lk(mu_);
      current = status_.state;
    }
    if (current == State::kFault) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kPumpMs));
      FailPendingQueue();
      continue;
    }

    if (phase == Phase::kConnect) {
      if (current != State::kReconnecting) {
        SetState(State::kOpening, "opening " + opt_.port);
      }
      if (!TryOpen()) {
        std::string err;
        {
          std::lock_guard lk(mu_);
          err = status_.last_error;
        }
        SetState(State::kReconnecting, err);
        BackoffWait();
        continue;
      }
      if (current == State::kReconnecting) {
        SetState(State::kOpening, opt_.port + " is back");
      }
      phase = Phase::kIdentify;
    }

    if (phase == Phase::kIdentify) {
      SetState(State::kIdentifying, "querying firmware");
      if (!Identify()) {
        if (!port_) {
          SetState(State::kReconnecting, "port lost while identifying");
          phase = Phase::kConnect;
        } else {
          SetState(State::kFault,
                   "no reply to PQTMVERNO at any baud on " + opt_.port);
        }
        continue;
      }
      if (opt_.apply_config) {
        SetState(State::kConfiguring, "applying configuration");
        if (!Configure()) {
          if (!port_) {
            SetState(State::kReconnecting, "port lost while configuring");
            phase = Phase::kConnect;
          } else {
            std::string err;
            {
              std::lock_guard lk(mu_);
              err = status_.last_error;
            }
            SetState(State::kFault, err);
          }
          continue;
        }
      }
      SetState(State::kStreaming, "receiver ready");
      backoff_ms_ = opt_.backoff_min_ms;
      had_connection_ = true;
      last_fix_t_ = std::max(last_fix_t_, MonotonicNs());
      phase = Phase::kStream;
    }

    if (phase == Phase::kStream) {
      while (!StopRequested() && port_ && !reidentify_) {
        StreamOnce();
      }
      if (reidentify_) {
        reidentify_ = false;
        phase = Phase::kIdentify;
      } else if (!port_) {
        phase = Phase::kConnect;
      }
    }
  }

  port_.reset();
  FailPendingQueue();
  SetState(State::kStopped, "stopped");
}

bool Lg290pReceiver::TryOpen() {
  std::string err;
  port_ = SerialPort::Open(opt_.port, opt_.baud, err);
  if (!port_) {
    std::lock_guard lk(mu_);
    status_.last_error = err;
    return false;
  }
  std::array<uint8_t, 2048> junk{};
  const int64_t drain_until = MonotonicNs() + kOpenDrainNs;
  while (MonotonicNs() < drain_until && port_->Read(junk, 20) >= 0) {
  }
  framer_.Reset();
  epoch_.Reset();
  last_utc_ = -1.0;
  std::lock_guard lk(mu_);
  status_.baud = opt_.baud;
  if (had_connection_) {
    ++status_.reconnects;
  }
  return true;
}

void Lg290pReceiver::BackoffWait() {
  const bool absent = !PathExists(opt_.port);
  const int64_t until = MonotonicNs() + MsToNs(backoff_ms_);
  while (!StopRequested() && MonotonicNs() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (absent && PathExists(opt_.port)) {
      break;  // replugged: retry at once
    }
  }
  backoff_ms_ = std::min(backoff_ms_ * 2, opt_.backoff_max_ms);
}

bool Lg290pReceiver::Pump(int timeout_ms) {
  if (!port_) {
    return false;
  }
  std::array<uint8_t, 2048> buf{};
  const int n = port_->Read(buf, timeout_ms);
  if (n < 0) {
    port_.reset();
    return false;
  }
  if (n == 0) {
    return true;
  }
  const int64_t t = MonotonicNs();
  const std::span<const uint8_t> data(buf.data(), static_cast<size_t>(n));
  if (on_raw_) {
    on_raw_(data);
  }
  framer_.Feed(data, [&](const Frame& f) { OnFrame(f, t); });
  std::lock_guard lk(mu_);
  status_.bytes += static_cast<uint64_t>(n);
  status_.framer = framer_.Stats();
  return true;
}

void Lg290pReceiver::OnFrame(const Frame& f, int64_t t_arrival) {
  if (f.kind == FrameKind::kRtcm) {
    if (on_rtcm_) {
      on_rtcm_(f.bytes);
    }
    return;
  }
  const std::string_view line = f.Text();
  if (on_nmea_) {
    on_nmea_(line);
  }

  if (line.starts_with("$PQTM") && (awaiting_ != nullptr || in_flight_)) {
    std::array<std::string_view, nmea::kMaxFields> fields{};
    if (const auto reply = nmea::ParseReply(line, fields)) {
      if (awaiting_ != nullptr && lg290p::Matches(*awaiting_, *reply)) {
        *awaiting_reply_ = nmea::Own(*reply);
        awaiting_ = nullptr;
      } else if (in_flight_ &&
                 in_flight_->written >= in_flight_->cmd.text.size() &&
                 lg290p::Matches(in_flight_->cmd, *reply)) {
        in_flight_->promise.set_value(nmea::Own(*reply));
        in_flight_.reset();
      }
    }
  }

  Fix fix{};
  if (!epoch_.Feed(line, t_arrival, fix)) {
    return;
  }
  // A receiver that restarted on its own (power glitch, watchdog) jumps back
  // in time; re-identify so configuration that did not persist is re-applied.
  const bool midnight = last_utc_ > 86000.0 && fix.utc_s < 400.0;
  if (last_utc_ >= 0.0 && fix.utc_s < last_utc_ - 1.0 && !midnight) {
    reidentify_ = true;
  }
  last_utc_ = fix.utc_s;
  last_fix_t_ = fix.t_arrival_ns;
  if (prev_fix_arrival_ > 0 && opt_.stale_after_ms <= 0) {
    const double gap_ms =
        static_cast<double>(fix.t_arrival_ns - prev_fix_arrival_) / 1e6;
    if (gap_ms > 0.0 && gap_ms < 5000.0) {
      interval_ms_ =
          interval_ms_ > 0.0 ? 0.8 * interval_ms_ + 0.2 * gap_ms : gap_ms;
    }
  }
  prev_fix_arrival_ = fix.t_arrival_ns;
  // "Corrected" means corrected by *our* RTCM: without recent injection the
  // receiver's differential solution comes from elsewhere (SBAS, PPP), whose
  // frame is not the declared correction frame.
  if (fix.frame == FixFrame::kCorrected) {
    const int64_t allowed =
        static_cast<int64_t>((static_cast<double>(fix.diff_age_s) + 5.0) * 1e9);
    if (last_inject_ns_ == 0 || fix.t_arrival_ns - last_inject_ns_ > allowed) {
      fix.frame = FixFrame::kAutonomous;
    }
  }
  State current;
  {
    std::lock_guard lk(mu_);
    latest_ = fix;
    ++status_.epochs;
    current = status_.state;
  }
  if (on_fix_) {
    on_fix_(fix);
  }
  if (current == State::kStale) {
    SetState(State::kStreaming, "epochs resumed");
  }
}

bool Lg290pReceiver::Execute(const lg290p::Command& cmd,
                             std::optional<nmea::OwnedReply>& reply) {
  reply.reset();
  for (int i = 0; i < 16 && ring_tail_.load() != ring_head_.load(); ++i) {
    DrainInject();
  }
  if (!port_) {
    return false;
  }
  const auto* p = reinterpret_cast<const uint8_t*>(cmd.text.data());
  size_t done = 0;
  const int64_t deadline = MonotonicNs() + MsToNs(opt_.command_timeout_ms);
  while (done < cmd.text.size()) {
    size_t w = 0;
    if (!port_->WriteSome(std::span(p + done, cmd.text.size() - done), w)) {
      port_.reset();
      return false;
    }
    done += w;
    if (done < cmd.text.size()) {
      if (MonotonicNs() > deadline) {
        return true;  // could not even send: treat as timeout
      }
      if (!Pump(5)) {
        return false;
      }
    }
  }
  if (cmd.reply_to.empty()) {
    return true;
  }
  awaiting_ = &cmd;
  awaiting_reply_ = &reply;
  while (awaiting_ != nullptr && !StopRequested()) {
    const int64_t left_ms = (deadline - MonotonicNs()) / kNsPerMs;
    if (left_ms <= 0) {
      break;
    }
    if (!Pump(static_cast<int>(std::min<int64_t>(left_ms, kPumpMs)))) {
      awaiting_ = nullptr;
      return false;
    }
  }
  awaiting_ = nullptr;
  return true;
}

bool Lg290pReceiver::Identify() {
  std::vector<int> bauds{opt_.baud};
  for (int b : opt_.baud_candidates) {
    if (std::find(bauds.begin(), bauds.end(), b) == bauds.end()) {
      bauds.push_back(b);
    }
  }
  for (int round = 0; round < kIdentifyRounds && !StopRequested(); ++round) {
    for (int b : bauds) {
      if (StopRequested()) {
        return false;
      }
      if (!port_) {
        return false;
      }
      if (port_->Baud() != b) {
        if (!port_->SetBaud(b)) {
          continue;
        }
        port_->Flush();
        framer_.Reset();
      }
      std::optional<nmea::OwnedReply> reply;
      if (!Execute(lg290p::QueryVersion(), reply)) {
        return false;
      }
      if (reply && reply->ok && !reply->values.empty()) {
        std::lock_guard lk(mu_);
        status_.firmware = reply->values[0];
        status_.baud = b;
        return true;
      }
    }
  }
  return false;
}

bool Lg290pReceiver::Configure() {
  for (int pass = 0; pass < 2; ++pass) {
    lg290p::ReceiverConfig have;
    for (const auto& cmd : lg290p::ReadCommands(opt_.config)) {
      std::optional<nmea::OwnedReply> reply;
      for (int attempt = 0; attempt <= opt_.command_retries && !reply;
           ++attempt) {
        if (!Execute(cmd, reply)) {
          return false;
        }
      }
      if (reply && reply->ok) {
        lg290p::ApplyReadReply(*reply, have);
      }
    }
    if (have.fix_interval_ms) {
      interval_ms_ = *have.fix_interval_ms;  // until epochs measure it
    }
    std::vector<std::string> unknown;
    const auto plan = lg290p::PlanConfig(opt_.config, have, &unknown);
    if (!unknown.empty()) {
      std::string names;
      for (const auto& u : unknown) {
        names += (names.empty() ? "" : ", ") + u;
      }
      std::lock_guard lk(mu_);
      status_.last_error = "left unchanged (unreadable or invalid): " + names;
    }
    if (plan.empty()) {
      return true;
    }
    if (pass == 1) {
      std::string names;
      for (const auto& c : plan) {
        names += (names.empty() ? "" : ", ") + c.reply_to;
      }
      std::lock_guard lk(mu_);
      status_.last_error = "receiver did not keep after reset: " + names;
      return true;
    }
    bool reset = false;
    for (const auto& cmd : plan) {
      bool ok = false;
      std::string why = "no reply";
      for (int attempt = 0; attempt <= opt_.command_retries && !ok; ++attempt) {
        std::optional<nmea::OwnedReply> reply;
        if (!Execute(cmd, reply)) {
          return false;
        }
        if (cmd.reply_to.empty()) {
          ok = true;
        } else if (reply && reply->ok) {
          ok = true;
        } else if (reply) {
          why = "error " + std::to_string(reply->error_code);
        }
      }
      if (!ok) {
        std::string text = cmd.text;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
          text.pop_back();
        }
        std::lock_guard lk(mu_);
        status_.last_error = "receiver refused " + text + " (" + why + ")";
        return false;
      }
      if (cmd.reply_to.empty()) {
        reset = true;  // PQTMSRR
      }
    }
    if (!reset) {
      return true;
    }
    // Let the receiver reboot, then talk to it again.
    const int64_t until = MonotonicNs() + 300'000'000;
    while (MonotonicNs() < until && !StopRequested()) {
      if (!Pump(kPumpMs)) {
        return false;
      }
    }
    epoch_.Reset();
    last_utc_ = -1.0;
    if (!Identify()) {
      return false;
    }
  }
  return true;
}

void Lg290pReceiver::DrainInject() {
  if (!port_) {
    return;
  }
  const size_t head = ring_head_.load(std::memory_order_acquire);
  size_t tail = ring_tail_.load(std::memory_order_relaxed);
  size_t budget = kInjectBudget;
  uint64_t sent = 0;
  while (tail != head && budget > 0) {
    const size_t off = tail % kRingSize;
    const size_t chunk = std::min({head - tail, kRingSize - off, budget});
    size_t w = 0;
    const bool ok =
        port_->WriteSome(std::span<const uint8_t>(ring_.get() + off, chunk), w);
    tail += w;
    budget -= w;
    sent += w;
    if (!ok || w < chunk) {
      break;
    }
  }
  ring_tail_.store(tail, std::memory_order_release);
  if (sent > 0) {
    last_inject_ns_ = MonotonicNs();
    std::lock_guard lk(mu_);
    status_.rtcm_injected += sent;
  }
}

void Lg290pReceiver::WriteInFlight() {
  // One non-blocking write per loop iteration: a device that stops reading
  // must not stall the I/O thread (fixes, stale detection, Stop()).
  const std::string& text = in_flight_->cmd.text;
  if (!port_ || in_flight_->written >= text.size()) {
    return;
  }
  const auto* p = reinterpret_cast<const uint8_t*>(text.data());
  size_t w = 0;
  if (!port_->WriteSome(
          std::span(p + in_flight_->written, text.size() - in_flight_->written),
          w)) {
    port_.reset();
    return;
  }
  in_flight_->written += w;
}

void Lg290pReceiver::ServeQueue() {
  if (!in_flight_) {
    if (ring_tail_.load() != ring_head_.load()) {
      return;  // never split an RTCM frame
    }
    std::lock_guard lk(mu_);
    if (queue_.empty()) {
      return;
    }
    in_flight_.emplace(std::move(queue_.front()));
    queue_.pop_front();
    in_flight_deadline_ = MonotonicNs() + MsToNs(opt_.command_timeout_ms);
  }
  WriteInFlight();
  const bool sent = in_flight_->written >= in_flight_->cmd.text.size();
  if (!port_ || MonotonicNs() > in_flight_deadline_ ||
      (sent && in_flight_->cmd.reply_to.empty())) {
    in_flight_->promise.set_value(std::nullopt);
    in_flight_.reset();
  }
}

void Lg290pReceiver::FailPendingQueue() {
  if (in_flight_) {
    in_flight_->promise.set_value(std::nullopt);
    in_flight_.reset();
  }
  std::deque<Pending> q;
  {
    std::lock_guard lk(mu_);
    q.swap(queue_);
  }
  for (auto& p : q) {
    p.promise.set_value(std::nullopt);
  }
}

int64_t Lg290pReceiver::StaleAfterNs() const {
  if (opt_.stale_after_ms > 0) {
    return MsToNs(opt_.stale_after_ms);
  }
  const double interval = interval_ms_ > 0.0 ? interval_ms_ : 1000.0;
  return static_cast<int64_t>(std::max(500.0, 3.0 * interval) * 1e6);
}

void Lg290pReceiver::StreamOnce() {
  ServeQueue();
  DrainInject();
  if (!Pump(kPumpMs)) {
    if (in_flight_) {
      in_flight_->promise.set_value(std::nullopt);
      in_flight_.reset();
    }
    Fix fix{};
    epoch_.Flush(fix);  // a half epoch from before the loss is not published
    SetState(State::kReconnecting, "port lost: " + opt_.port);
    return;
  }
  const int64_t now = MonotonicNs();
  if (now - last_fix_t_ > StaleAfterNs()) {
    State current;
    {
      std::lock_guard lk(mu_);
      current = status_.state;
    }
    if (current == State::kStreaming) {
      Fix fix{};
      if (epoch_.Flush(fix)) {
        // The receiver went quiet mid-epoch: publish what arrived.
        {
          std::lock_guard lk(mu_);
          latest_ = fix;
          ++status_.epochs;
        }
        if (on_fix_) {
          on_fix_(fix);
        }
      }
      SetState(
          State::kStale,
          "no epoch for " + std::to_string(StaleAfterNs() / kNsPerMs) + " ms");
    }
  }
}

}  // namespace lgrtk
