// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
/// @file
/// lgrtkd: one receiver (plus optional NTRIP) served over gpsd JSON, a raw
/// tap, chrony and a control socket. The receiver's callbacks only post work
/// to the event loop; every socket is served by the loop thread.
#ifndef LGRTK_SRC_DAEMON_DAEMON_H_
#define LGRTK_SRC_DAEMON_DAEMON_H_

#include <memory>
#include <string>

#include "daemon/chrony.h"
#include "daemon/config.h"
#include "daemon/control.h"
#include "daemon/event_loop.h"
#include "daemon/gpsd_server.h"
#include "daemon/raw_tap.h"
#include "daemon/sky.h"
#include "lgrtk/ntrip.h"
#include "lgrtk/receiver.h"

namespace lgrtk::daemon {

/// The daemon. Start() binds and starts everything, Run() serves until
/// Quit(), Stop() shuts the receiver and NTRIP down.
class Daemon {
 public:
  /// `config` as loaded; the NTRIP password is read in Start().
  explicit Daemon(DaemonConfig config);
  ~Daemon();
  Daemon(const Daemon&) = delete;
  Daemon& operator=(const Daemon&) = delete;

  /// Binds every socket and starts the receiver (and NTRIP). False with
  /// `err` when a socket cannot be bound or the password cannot be read.
  bool Start(std::string* err);
  /// Serves until Quit(); also returns on SIGTERM/SIGINT when
  /// `handle_signals` (the signals must be blocked in every thread first).
  void Run(bool handle_signals);
  /// Makes Run() return. Thread-safe.
  void Quit() { loop_.Quit(); }
  /// Stops NTRIP and the receiver. Call after Run() returned.
  void Stop();

 private:
  void StartDevices();
  void StopDevices();
  void OnFix(const Fix& fix, int64_t arrival_realtime_ns);
  void OnState(const StatusEvent& ev);
  [[nodiscard]] nlohmann::json StatusJson() const;
  void UpdateSystemdStatus() const;

  DaemonConfig cfg_;
  std::string ntrip_password_;
  EventLoop loop_;
  std::unique_ptr<GpsdServer> gpsd_;
  std::unique_ptr<RawTap> raw_;
  std::unique_ptr<ChronySock> chrony_;
  std::unique_ptr<ControlServer> control_;
  SkyTracker sky_;
  // Owned by the loop thread; recreated by apply-default.
  std::unique_ptr<Lg290pReceiver> rx_;
  std::unique_ptr<NtripClient> ntrip_;
  State state_ = State::kDisconnected;
};

}  // namespace lgrtk::daemon

#endif  // LGRTK_SRC_DAEMON_DAEMON_H_
