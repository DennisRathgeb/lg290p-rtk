# lg290p-rtk Milestone 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A C++20 library `lgrtk` and CLI `lgrtk` that drive a Quectel LG290P over USB serial
through an explicit state machine, deliver one normalised `Fix` per epoch, stream NTRIP corrections
into the receiver and project fixes with PROJ; tagged `v0.1.0`.

**Architecture:** Layered, each layer testable alone: bytes (`SerialPort`) → frames
(`StreamFramer`) → sentences (`nmea::Parse*`) → epochs (`EpochAssembler` → `Fix`) → device
(`Lg290pReceiver`, state machine on its own I/O thread, `Lg290p` command catalogue) — plus two
independent services, `NtripClient` (corrections) and `Projection`/`LocalFrame` (PROJ). A simulated
LG290P on a pseudo-terminal drives every integration test, so CI needs no hardware.

**Tech Stack:** C++20, CMake ≥ 3.22 + Ninja, GoogleTest (system or FetchContent), PROJ ≥ 8.2
(optional component), POSIX termios/openpty/poll, sockets. OpenSSL deferred (NTRIP TLS is M2).

**Spec:** [docs/design.md](design.md) (§9.1 holds the builder-check resolutions). Decisions taken
without Dennis: [docs/decisions.md](decisions.md).

## Global Constraints

- Linux only; C++20; must build with gcc 11.4 and current gcc/clang; `-Wall -Wextra -Wpedantic -Werror` in CI.
- Google C++ style: `.clang-format` `BasedOnStyle: Google`, naming per `.clang-tidy`; headers `.h`, sources `.cc`, guards `LGRTK_<PATH>_H_`.
- Namespace `lgrtk`; public headers under `include/lgrtk/`; library target `lgrtk::lgrtk`, optional `lgrtk::projection` (needs PROJ).
- Hot path (read → frame → parse → assemble → callback) allocates nothing after `Start()`.
- Fix timestamps: `t_arrival_ns` = `CLOCK_MONOTONIC` right after the read that completed the epoch's last sentence, before parsing.
- I/O thread reads with a 100 ms timeout; `Stop()` joins within 1 s; corrections written ≤ 4 KiB per loop iteration.
- Config apply is read-back-and-diff; `PQTMSAVEPAR` + `PQTMSRR` only when a reset-requiring setting changed.
- Credentials never logged.
- Licence MIT; copyright "2026 Dennis Rathgeb".

## Review Focus

1. A read that ends mid-sentence or mid-RTCM-frame must neither drop nor duplicate it → Task 2 `FramerTest.SplitsAcrossEveryByteBoundary`.
2. Garbage bytes (wrong baud, boot noise, a `$` inside binary RTCM) must resynchronise without losing the next valid frame → Task 2 `FramerTest.ResyncsAfterGarbage`, `FramerTest.DollarInsideRtcmPayload`.
3. An epoch with GGA but no GST (default config, old firmware) must still yield a fix with sigma flagged absent → Task 4 `EpochTest.GgaAloneYieldsFixWithoutSigma`.
4. Unplug while streaming must report `kReconnecting` and come back to `kStreaming` on replug without restart, and the latest-fix snapshot must report its age → Task 7 `ReceiverTest.UnplugReplugRecovers`.
5. A caster answering 401 must stop retrying (no hammering) and say why → Task 9 `NtripTest.AuthFailureStops`.

---

### Task 1: Repository scaffold

**Files:** Create `CMakeLists.txt`, `CMakePresets.json` (presets `dev` Debug+ASan/UBSan, `release`), `cmake/Warnings.cmake`, `include/lgrtk/version.h` (`kVersion = "0.1.0"`), `src/version.cc`, `tests/CMakeLists.txt`, `tests/version_test.cc`, `LICENSE` (MIT), `.gitignore`, `.github/workflows/ci.yml` (Ubuntu 22.04 gcc-11 and 24.04; installs `libgtest-dev libproj-dev`; configure, build, ctest; clang-format check).

- [ ] Test `VersionTest.MatchesProject`: `EXPECT_STREQ(lgrtk::kVersion, "0.1.0")`.
- [ ] Build `cmake --preset dev && cmake --build --preset dev && ctest --preset dev` → PASS. Commit "scaffold".

### Task 2: StreamFramer (NMEA + RTCM3)

**Files:** `include/lgrtk/framer.h`, `src/framer.cc`, `tests/framer_test.cc`.

**Interfaces — Produces:**
```cpp
enum class FrameKind : uint8_t { kNmea, kRtcm };
struct Frame { FrameKind kind; std::span<const uint8_t> bytes; };  // NMEA without CR/LF, RTCM incl. header+CRC
class StreamFramer {
 public:
  template <class F> void Feed(std::span<const uint8_t> data, F&& on_frame);  // calls on_frame(const Frame&)
  struct Counters { uint64_t nmea, rtcm, bad_nmea, bad_rtcm, skipped_bytes, oversize; };
  const Counters& counters() const;
};
uint32_t Crc24q(std::span<const uint8_t>);
bool NmeaChecksumOk(std::string_view line);  // "$...*HH"
```
Fixed 4 KiB internal buffer; NMEA max 255 chars (Quectel PQTM lines can exceed 82), RTCM max 1029 bytes; bad frames counted and skipped byte-wise to the next `$`/`0xD3`.

- [ ] Tests: `SplitsAcrossEveryByteBoundary` (a GGA + an RTCM 1005 frame fed split at every offset → exactly 2 frames each time), `ResyncsAfterGarbage`, `DollarInsideRtcmPayload` (RTCM payload containing `$GPGGA` bytes → 1 RTCM frame, 0 NMEA), `BadCrcCounted`, `BadNmeaChecksumCounted`, `Crc24qKnownVector` (`Crc24q("123456789") == 0xCDE703`).
- [ ] Implement, PASS, commit.

### Task 3: NMEA and PQTM parsers

**Files:** `include/lgrtk/nmea.h`, `src/nmea.cc`, `tests/nmea_test.cc`. Start from ar-stack `code/core/src/devices/gnss/nmea.cc` (same author; GGA/GST/Validate/SplitFields/SentenceType), rename namespace to `lgrtk::nmea`.

**Interfaces — Produces:** `std::optional<Gga> ParseGga(std::string_view)`, `ParseGst`, `ParseRmc` (utc, date y/m/d, valid, speed_mps, course_deg), `ParseZda`, `ParseVtg`, `ParseGsa` (fix 1/2/3, pdop/hdop/vdop, system id), `ParseGsv` (system/signal id, total msgs, msg no, sats in view, up to 4 × {prn, elev, az, cn0}), `ParseEpe` (`PQTMEPE`: n,e,d,2d,3d m), `ParseVerno` (version, date, time strings as `std::string_view` into the line), `ParseSvinStatus`, and `std::optional<Reply> ParseReply(std::string_view)` where `struct Reply { std::string_view command; bool ok; int error_code; std::span<const std::string_view> values; }` for `$PQTMxxx,OK[,..]` / `$PQTMxxx,ERROR,<code>` (values in a caller-provided field array). Talker-agnostic (`GN`, `GP`, `GA`, …).

- [ ] Tests with the protocol spec's example lines (e.g. `$GNGST,013902.500,5.387,3.734,2.172,143.088,2.836,3.258,9.274*74`) and lines from `tests/data/probe1.nmea` (capture from the bench, firmware `LG290P03AANR01A06S`): every parser on a valid line, empty fields → defaults, wrong type → nullopt, `ParseReply("$PQTMCFGFIXRATE,OK*..")`, `ParseReply("$PQTMCFGMSGRATE,ERROR,1*..")`.
- [ ] Implement, PASS, commit.

### Task 4: Fix and EpochAssembler

**Files:** `include/lgrtk/fix.h`, `include/lgrtk/epoch.h`, `src/epoch.cc`, `tests/epoch_test.cc`.

**Interfaces — Produces:**
```cpp
enum class FixType : uint8_t { kNone, kSingle, kDgnss, kPps, kRtkFixed, kRtkFloat, kDeadReckoning, kManual, kSimulation };
enum class Frame : uint8_t { kAutonomous, kCorrected };          // spec §9.1 item 2
enum class SigmaSource : uint8_t { kNone, kGst, kEpe };
struct Fix {
  int64_t t_arrival_ns; double utc_s; int32_t date_ymd;            // 20261110, 0 unknown
  double lat_deg, lon_deg, h_ell_m, geoid_sep_m;                    // h_ell = alt_msl + geoid_sep
  float sigma_e_m, sigma_n_m, sigma_u_m; SigmaSource sigma_source;
  float vel_e_mps, vel_n_mps, vel_u_mps; bool vel_valid;
  float hdop, pdop, diff_age_s; uint16_t station_id; uint8_t sats;
  FixType type; Frame frame; uint32_t seq;
};
FixType FixTypeFromGgaQuality(uint8_t);  // 0 none,1 single,2 dgnss,3 pps,4 rtk fixed,5 rtk float,6 dr,7 manual,8 sim
class EpochAssembler {
 public:
  // Returns true and fills `out` when an epoch closes (a sentence with a newer UTC arrives, or
  // GGA plus its same-epoch GST/EPE/RMC all seen). t_arrival of the closing sentence.
  bool Feed(std::string_view nmea_line, int64_t t_arrival_ns, Fix& out) noexcept;
  bool Flush(Fix& out) noexcept;  // close the pending epoch (used on stale/stop)
};
```
Rules: GGA is mandatory for a fix; GST preferred over EPE for sigma (GST lat/lon/alt → n/e/u); velocity from RMC speed+course (horizontal only, `vel_u` 0); `frame = kCorrected` iff type ∈ {kDgnss, kRtkFixed, kRtkFloat} and diff age present; `seq` increments per emitted fix. Epoch closes immediately once GGA+RMC and (GST or EPE when they were seen in the previous epoch) are present, otherwise when the next epoch's first sentence arrives — so latency is one sentence, not one epoch.

- [ ] Tests: `GgaAloneYieldsFixWithoutSigma`, `GstBeforeOrAfterGgaPairs`, `ClosesWithoutWaitingWhenComplete` (GGA+RMC+GST of one epoch → fix emitted on the 3rd line once GST has been seen before), `CorrectedFrameNeedsDiffAge`, `SeqIncrements`, `ReplayCaptureGivesTenHz` (`tests/data/probe1.nmea` → ~300 fixes, all `kNone`).
- [ ] Implement, PASS, commit.

### Task 5: LG290P command catalogue

**Files:** `include/lgrtk/lg290p.h`, `src/lg290p.cc`, `tests/lg290p_test.cc`.

**Interfaces — Produces:** `std::string BuildCommand(std::string_view body)` → `"$<body>*HH\r\n"`;
`struct Command { std::string text; std::string reply_to; bool needs_reset; };` and builders in
`namespace lgrtk::lg290p`: `QueryVersion()`, `QueryUniqueId()`, `SetFixRate(int ms)` (50..1000 ms, needs reset), `GetFixRate()`,
`SetMessageRate(std::string_view msg, int rate, int ver = 0)`, `GetMessageRate(msg)`, `SetNmeaDecimals(utc,pos,alt,dop,spd,cog)`,
`SetRtk(int diff_mode, int rel_mode, int timeout_s)`, `SetConstellations(bool gps, glo, gal, bds, qzss, navic)` (reset),
`SetElevationMask(double deg)`, `SetCnrMask(double dbhz)`, `SetReceiverMode(Mode::kRover|kBase)` (reset), `SurveyIn(int seconds, double acc_m)`, `FixedBase(double x, y, z)`,
`SetPps(...)`, `SetUartBaud(int port, int baud)`, `SaveParams()`, `SoftwareReset()`, `Restart(Start::kHot|kWarm|kCold)`, `RestoreDefaults()`.
`struct ReceiverConfig` (desired settings, each `std::optional`) and `std::vector<Command> PlanConfig(const ReceiverConfig& want, const ReceiverConfig& have)` that returns only differing writes plus a trailing `SaveParams`/`SoftwareReset` when any write needs a reset (or save only when non-reset settings changed).
Command strings and reply names per Quectel protocol spec v1.1 (see design §10 and `research/notes/rtk-gnss-lg290p.md` §2 in ar-stack).

- [ ] Tests: `BuildCommandChecksum` (`BuildCommand("PQTMVERNO") == "$PQTMVERNO*58\r\n"`, `BuildCommand("PQTMSAVEPAR") == "$PQTMSAVEPAR*5A\r\n"`, `"PQTMSRR"` → `*4B`), `SetFixRateRange` (49 and 1001 rejected), `PlanConfigEmptyWhenEqual`, `PlanConfigAddsSaveAndResetOnlyWhenNeeded`.
- [ ] Implement, PASS, commit.

### Task 6: SerialPort and simulated LG290P

**Files:** `include/lgrtk/serial.h`, `src/serial.cc`, `tests/sim/sim_lg290p.h`, `tests/sim/sim_lg290p.cc`, `tests/serial_test.cc`.

**Interfaces — Produces:**
`class SerialPort { static std::optional<SerialPort> Open(const std::string& path, int baud, std::string& err); int Read(std::span<uint8_t>, int timeout_ms); /* >0 bytes, 0 timeout, -1 error/hangup */ bool WriteSome(std::span<const uint8_t>, size_t& written); bool SetBaud(int); int fd() const; }` (raw 8N1, `TIOCEXCL`, `O_NONBLOCK`, poll-based).
Test helper `SimLg290p`: owns an `openpty` pair (exposes the slave path, can be "unplugged" = close master and unlink a symlink path, and "replugged"), emits a scripted 10 Hz NMEA epoch (GGA+RMC, GST when enabled) on its own thread, answers `PQTMVERNO`, `PQTMCFGFIXRATE`, `PQTMCFGMSGRATE`, `PQTMSAVEPAR`, `PQTMSRR` (simulated reboot: 300 ms silence), can be told to NAK a command, and records RTCM bytes written to it.

- [ ] Tests: `SerialTest.ReadsFromSim`, `SerialTest.HangupReturnsMinusOne`, `SerialTest.SecondOpenFailsExclusive`.
- [ ] Implement, PASS, commit.

### Task 7: Lg290pReceiver state machine

**Files:** `include/lgrtk/receiver.h`, `src/receiver.cc`, `tests/receiver_test.cc`.

**Interfaces — Consumes:** Tasks 2–6. **Produces:**
```cpp
enum class State : uint8_t { kDisconnected, kOpening, kIdentifying, kConfiguring, kStreaming, kStale, kReconnecting, kFault, kStopped };
const char* ToString(State);
struct StatusEvent { State from, to; std::string reason; int64_t t_ns; };
struct ReceiverOptions { std::string port = "/dev/ttyACM0"; int baud = 460800; std::vector<int> baud_candidates{460800, 921600, 115200, 230400, 9600};
  ReceiverConfig config; bool apply_config = true; int stale_after_ms = 500; int backoff_min_ms = 250, backoff_max_ms = 5000; int command_timeout_ms = 1000; int command_retries = 2; };
struct ReceiverStatus { State state; std::string firmware, port; int baud; uint64_t bytes, epochs, reconnects; StreamFramer::Counters framer; int64_t last_fix_age_ns; std::string last_error; };
class Lg290pReceiver final : public CorrectionSink {
 public:
  explicit Lg290pReceiver(ReceiverOptions);
  void OnFix(std::function<void(const Fix&)>); void OnStatus(std::function<void(const StatusEvent&)>);
  void OnNmea(std::function<void(std::string_view)>); void OnRtcm(std::function<void(std::span<const uint8_t>)>);
  void OnRaw(std::function<void(std::span<const uint8_t>)>);           // set before Start()
  bool Start(); void Stop();
  ReceiverStatus Status() const; std::optional<Fix> LatestFix() const;  // seqlock snapshot
  std::future<std::optional<nmea::OwnedReply>> Send(lg290p::Command);  // queued, executed on the I/O thread
  bool Inject(std::span<const uint8_t> rtcm) override;                    // bounded SPSC byte ring 64 KiB; false if full
};
struct CorrectionSink { virtual bool Inject(std::span<const uint8_t>) = 0; virtual ~CorrectionSink() = default; };  // in include/lgrtk/correction_sink.h
```
Transitions exactly as design §4; `kFault` after a command is NAKed `command_retries+1` times (reason names command and error code) — the I/O thread then waits for `Stop()` or a new `Start()`. Port disappearance detected by `Read` returning -1 or `access(port)` failing; reopen attempts with backoff, but immediately when the path reappears (poll `access()` every 100 ms during backoff).

- [ ] Tests (all with `SimLg290p`): `StreamsFixes` (state reaches kStreaming, ≥ 5 fixes in 1 s, firmware string from sim), `DetectsBaud` (sim at 115200, options.baud 460800 → identified at 115200), `AppliesOnlyDifferences` (second Start: no writes sent), `StaleWhenSilentThenRecovers`, `UnplugReplugRecovers` (unplug → kReconnecting within 300 ms; replug → kStreaming within 2 s; LatestFix age grows meanwhile), `NakLeadsToFault`, `InjectReachesDevice` (bytes injected appear at sim), `StopJoinsFast` (< 1 s).
- [ ] Implement, PASS (also under ASan/UBSan preset), commit.

### Task 8: Projection and LocalFrame (optional PROJ component)

**Files:** `include/lgrtk/projection.h`, `src/projection.cc`, `tests/projection_test.cc`. CMake option `LGRTK_WITH_PROJ` (ON if found).

**Interfaces — Produces:**
```cpp
class Projection {  // movable, not copyable; one PJ_CONTEXT per object; not thread-safe, one per thread
 public:
  static std::optional<Projection> Create(std::string_view from_crs, std::string_view to_crs, std::string& err);  // always_xy normalised
  bool Forward(double lat_deg, double lon_deg, double h_m, double out[3]) const noexcept;   // to_crs x,y,z
  bool Inverse(const double in[3], double& lat_deg, double& lon_deg, double& h_m) const noexcept;
  std::string Description() const;  // the chosen PROJ pipeline, for logging
};
class LocalFrame {  // geodetic tangent-plane ENU around an origin on the GRS80/WGS84 ellipsoid; no PROJ needed
 public:
  LocalFrame(double lat0_deg, double lon0_deg, double h0_m);
  void ToEnu(double lat_deg, double lon_deg, double h_m, double enu[3]) const noexcept;
  void FromEnu(const double enu[3], double& lat_deg, double& lon_deg, double& h_m) const noexcept;
};
```
Create refuses when PROJ would fall back to a ballpark transformation (`proj_coordoperation_has_ballpark_transformation`) or needs a missing grid, with the grid name in `err`.

- [ ] Tests: `LocalFrameOriginIsZero`, `LocalFrameOneMetreEast` (|ENU − (1,0,0)| < 1 mm for a point 1 m east computed by inverse), `LocalFrameRoundTrip` (< 0.1 mm at 5 km), `ProjectionLv95Nalps` (`EPSG:4258`→`EPSG:2056`, 46.63 N 8.78 E → E 2 702 718.1, N 1 165 183.2 within 0.2 m — reference from ar-stack's `ntrip-and-swiss-coordinates.md`), `ProjectionRefusesUnknownCrs`, `ProjectionSpeed` (1e5 Forward calls < 1 s; asserts mean < 10 µs).
- [ ] Implement, PASS, commit.

### Task 9: NTRIP client

**Files:** `include/lgrtk/ntrip.h`, `src/ntrip.cc`, `tests/ntrip_test.cc`, `tests/sim/fake_caster.h/.cc`.

**Interfaces — Produces:**
```cpp
enum class NtripState : uint8_t { kIdle, kConnecting, kStreaming, kStalled, kBackoff, kAuthFailed, kNoPosition, kStopped };
struct NtripOptions { std::string host; int port = 2101; std::string mountpoint, user, password; int version = 2;  // 1 or 2
  int gga_interval_s = 10; bool send_gga = true; int stall_after_s = 10; int backoff_min_ms = 1000, backoff_max_ms = 60000; };
struct NtripStatus { NtripState state; uint64_t bytes, connects; int64_t last_data_age_ns; int http_status; std::string last_error; };
class NtripClient {
 public:
  NtripClient(NtripOptions, CorrectionSink& sink);
  void SetPosition(const Fix&);   // thread-safe; builds the GGA sent to VRS casters (no position yet → kNoPosition when send_gga)
  void OnStatus(std::function<void(NtripState, NtripState, const std::string&)>);
  bool Start(); void Stop(); NtripStatus Status() const;
  static std::optional<std::string> FetchSourcetable(const std::string& host, int port, int version, std::string& err);
};
```
v1 request `GET /mnt HTTP/1.0` + `User-Agent: NTRIP lgrtk/<ver>` + Basic auth, accepts `ICY 200 OK`; v2 `HTTP/1.1`, `Ntrip-Version: Ntrip/2.0`, chunked transfer decoding, `Ntrip-GGA` header for the first GGA. 401 → `kAuthFailed`, no retry until `Start()` again. Own thread, blocking socket with 1 s timeouts.

- [ ] Tests with `FakeCaster` (local TCP): `V2ChunkedStreamsToSink`, `V1IcyResponse`, `SendsGgaPeriodically`, `AuthFailureStops`, `ReconnectsAfterDrop` (backoff, `connects == 2`), `StalledWhenSilent`, `PasswordNotInStatusOrErrors`.
- [ ] Implement, PASS, commit.

### Task 10: `lgrtk` CLI

**Files:** `apps/lgrtk/main.cc` (+ small `apps/lgrtk/*.cc` per subcommand), `tests/cli_test.sh` (runs against the sim via a test binary `sim_lg290p_tool` that exposes a pty path).

Subcommands (all direct on the port; daemon mode in M2): `probe [--seconds N] [--raw FILE]` (same report as ar-stack `arstack gnss-probe`), `watch` (one line per fix: time, type, lat/lon/h, σ, sats, age), `status` (state machine events + counters for N s), `version`, `config get|set <key> <value>` (fix-rate, msg-rate, rtk, elevation-mask, cnr-mask, decimals, constellations), `reset hot|warm|cold|soft|factory` (factory requires `--yes`), `base survey-in SECONDS ACC | fixed X Y Z | rover`, `ntrip sourcetable HOST[:PORT]`, `ntrip run --host … --mount … --user … [--password-file F]` (receiver + NTRIP, watch output), `project --from CRS --to CRS LAT LON H`. Global `--port`, `--baud`. Exit codes: 0 ok, 1 runtime error, 2 usage.

- [ ] `cli_test.sh`: `probe --seconds 2` against the sim prints `firmware:` and `GGA`; `config set fix-rate 100` exits 0; `reset factory` without `--yes` exits 2.
- [ ] Implement, PASS, commit.

### Task 11: README, docs, examples, v0.1.0

**Files:** `README.md`, `docs/hardware.md` (the module: Waveshare board, CH343, pins, antenna, firmware, what each PQTM message gives, bench checklist), `docs/state-machine.svg`, `docs/architecture.svg`, `docs/images/README.md` (photo list Dennis takes: board top, board with antenna + USB, pin header close-up), `examples/print_fixes.cc`, `examples/ntrip_rover.cc`, `CHANGELOG.md`, `docs/decisions.md` complete.

README sections: what it is, photo placeholder + the module at a glance, quick start (build, plug in, `lgrtk probe`), library example (15 lines), state machine diagram, NTRIP, projection (Swiss LV95 example), troubleshooting (permissions `uucp`/`dialout`, ModemManager, baud, indoor no fix, GST missing on old firmware), status/roadmap, licence.
- [ ] Examples build in CI; `git tag v0.1.0`; push.

## Milestone 2 (later plan): `lgrtkd` gpsd-JSON daemon, raw tap, chrony SOCK, udev rule, systemd unit, TOML config, PKGBUILD, NTRIP TLS, NTRIP server upload for an own base.
