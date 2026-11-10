// Copyright 2026 Dennis Rathgeb. SPDX-License-Identifier: MIT
#ifndef LGRTK_TESTS_SIM_SIM_LG290P_H_
#define LGRTK_TESTS_SIM_SIM_LG290P_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace lgrtk::test {

/// A simulated LG290P behind a pseudo-terminal.
///
/// Exposes a stable path (a symlink in a temp directory) that can be
/// "unplugged" (master closed, symlink removed) and "replugged" (new pty,
/// same path). Emits 10 Hz epochs (GGA, RMC, GST/PQTMEPE when enabled),
/// answers the PQTM commands the driver uses, and records RTCM bytes it
/// receives. When the host's line speed differs from `baud`, it emits noise
/// and ignores commands, like a real receiver at a different UART speed.
class SimLg290p {
 public:
  explicit SimLg290p(int baud = 460800);
  ~SimLg290p();
  SimLg290p(const SimLg290p&) = delete;
  SimLg290p& operator=(const SimLg290p&) = delete;

  const std::string& path() const { return link_; }
  void Unplug();
  void Replug();
  void SetSilent(bool silent) { silent_ = silent; }
  void SetFixQuality(int q) { quality_ = q; }
  /// When a client opens the port, first send an old epoch, as the CH343
  /// bridge does with bytes left in its FIFO from the previous session.
  void SetBurstOnOpen(bool on) { burst_on_open_ = on; }
  /// Commands (e.g. "PQTMCFGMSGRATE") answered with ERROR,1.
  void Nak(const std::string& command);
  /// Every W command received, as the body without checksum.
  std::vector<std::string> Writes() const;
  void ClearWrites();
  std::vector<uint8_t> RtcmReceived() const;
  int resets() const { return resets_; }

 private:
  void OpenPty();
  void ClosePty();
  void Loop();
  void HandleCommand(const std::string& body);
  void Send(const std::string& line);
  bool HostBaudMatches() const;

  int baud_;
  std::string dir_, link_;
  int master_ = -1;
  int slave_keep_ = -1;
  std::atomic<bool> stop_{false}, silent_{false}, plugged_{false};
  std::atomic<int> quality_{1};
  std::atomic<bool> burst_on_open_{false};
  bool client_open_ = false;
  std::atomic<int> resets_{0};
  std::atomic<int64_t> silent_until_ns_{0};
  mutable std::mutex mu_;  // guards everything below and master_ swaps
  std::map<std::string, std::string> settings_;
  std::set<std::string> nak_;
  std::vector<std::string> writes_;
  std::vector<uint8_t> rtcm_;
  std::thread thread_;
};

}  // namespace lgrtk::test

#endif  // LGRTK_TESTS_SIM_SIM_LG290P_H_
