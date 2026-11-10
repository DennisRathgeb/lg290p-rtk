# lg290p-rtk Milestone 2 Implementation Plan: the `lgrtkd` daemon

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A daemon `lgrtkd` that owns one LG290P (plus optional NTRIP) and serves it system-wide:
gpsd JSON on TCP, a raw byte tap, a chrony SOCK refclock, a control socket for `lgrtk ctl`, with
systemd/udev/TOML/PKGBUILD around it; tagged `v0.2.0`.

**Architecture:** The library stays dependency-free. Daemon code lives in an internal static
library `lgrtkd_core` (`src/daemon/`, not installed) so it is unit-testable, and a thin
`apps/lgrtkd/main.cc`. One event-loop thread (`poll`) serves every socket; the receiver's
callbacks only enqueue events and wake the loop through an `eventfd`.

**Tech Stack:** C++20, toml++ v3.4.0 and nlohmann/json v3.11.3 (system package first, else
FetchContent; daemon only), POSIX sockets/poll/eventfd, systemd notify protocol written in-tree.

**Spec:** [docs/design.md](design.md) §5.2–5.4, §6, §7. Decisions: [docs/decisions.md](decisions.md)
(D2, D6, D12 and the new D23–D29 written in Task 8).

## Global Constraints

- Everything from the M1 plan's Global Constraints (gcc 11.4, Google style, `-Werror` in CI, guards `LGRTK_<PATH>_H_`, MIT).
- `lgrtk::lgrtk` gains no dependency; toml++/nlohmann only link into `lgrtkd_core`.
- gpsd JSON on `127.0.0.1:2947` by default (configurable); raw tap off by default; chrony off by default.
- Credentials never logged; NTRIP password from `password_file` or `$CREDENTIALS_DIRECTORY/ntrip-password`.
- Receiver callbacks never block on a socket: they copy into the event queue and return.
- A slow client never stalls the daemon: per-client output buffer capped (256 KiB), the client is dropped on overflow.
- `lgrtkd` exits 0 on SIGTERM/SIGINT within 2 s, 2 on a bad config, 1 on a runtime error at start (cannot bind).

## Review Focus

1. A gpsd client that connects and never reads must be dropped, not slow the others → Task 4 `GpsdServerTest.SlowClientIsDropped`.
2. A command line split across TCP reads, or several commands in one read, must be handled exactly once each → Task 4 `GpsdServerTest.CommandsSplitAcrossReads`.
3. A fix before the receiver knows the date (`date_ymd == 0`) must not produce a bogus `time` (1970/1980) in TPV or a chrony sample → Task 2 `GpsdJsonTest.NoTimeWithoutDate`, Task 5 `ChronyTest.NoSampleWithoutDate`.
4. An unknown key or wrong type in the TOML file must fail loudly at start (exit 2 with the key named), not be ignored → Task 1 `ConfigTest.UnknownKeyIsAnError`.
5. Unplugging the receiver must keep clients connected and report it (TPV `mode` 0 / DEVICES deactivated), then resume → Task 7 e2e `daemon_test.sh` unplug step.

---

### Task 1: Dependencies and the TOML configuration

**Files:** Create `src/daemon/config.{h,cc}`, `tests/daemon/config_test.cc`, `packaging/lgrtkd.toml` (documented example). Modify `CMakeLists.txt` (option `LGRTK_BUILD_DAEMON`, default = `LGRTK_BUILD_APPS`; deps; `lgrtkd_core`), `tests/CMakeLists.txt` (`lgrtkd_tests` executable).

**Interfaces — Produces:**
```cpp
namespace lgrtk::daemon {
struct NtripSection { bool enabled=false; std::string host; int port=2101; std::string mountpoint,
  user, password_file; int version=2; bool send_gga=true; };
struct DaemonConfig {
  ReceiverOptions receiver;            // port, baud, apply_config
  std::string gpsd_listen = "127.0.0.1:2947";
  std::string raw_listen;              // "" = off
  std::string chrony_sock;             // "" = off
  std::string control_sock = "/run/lgrtkd/control.sock";
  std::string device_name;             // reported in gpsd "device"; default = receiver.port
  NtripSection ntrip;
};
std::optional<DaemonConfig> ParseConfig(std::string_view toml, std::string* err);
std::optional<DaemonConfig> LoadConfig(const std::string& path, std::string* err);
std::optional<std::string> ReadNtripPassword(const NtripSection&, std::string* err);
bool ParseHostPort(std::string_view s, std::string* host, int* port);
}
```
TOML tables: `[receiver] port baud apply_config`, `[gpsd] listen device_name`, `[raw] listen`,
`[chrony] sock`, `[control] sock`, `[ntrip] enabled host port mountpoint user password_file version send_gga`.

- [ ] Tests: `ConfigTest.EmptyFileGivesDefaults`, `ConfigTest.ParsesAllSections` (values round-trip), `ConfigTest.UnknownKeyIsAnError` (err contains `"receiver.prot"`), `ConfigTest.WrongTypeIsAnError` (`baud = "fast"` → err names `receiver.baud`), `ConfigTest.NtripEnabledNeedsHostAndMount`, `ConfigTest.PasswordFromCredentialsDirectory` (setenv `CREDENTIALS_DIRECTORY`; trailing newline stripped), `ConfigTest.ParseHostPort` (`"[::1]:2947"`, `"0.0.0.0:1"`, `":2947"` → false).
- [ ] Run → FAIL (no target); implement; run → PASS; `packaging/lgrtkd.toml` parses in a test (`ConfigTest.ExampleFileParses`). Commit.

### Task 2: gpsd JSON encoding and command parsing

**Files:** Create `src/daemon/gpsd_json.{h,cc}`, `tests/daemon/gpsd_json_test.cc`.

**Interfaces — Produces:**
```cpp
namespace lgrtk::daemon {
std::optional<int64_t> FixUnixNs(const Fix&);           // nullopt when date_ymd == 0
std::string IsoTime(int64_t unix_ns);                     // "2026-11-10T12:00:00.100Z"
std::string VersionJson();                                // release lgrtkd <kVersion>, proto 3.14
std::string DevicesJson(const std::string& device, bool active, int baud);
std::string TpvJson(const Fix&, const std::string& device);
std::string GstJson(const Fix&, const std::string& device);   // "" when sigma_source == kNone
std::string NoFixTpvJson(const std::string& device);          // mode 0, sent on loss
struct Watch { bool enable=false, json=false, nmea=false; };
std::string WatchJson(const Watch&);
struct GpsdCommand { enum Kind { kVersion, kWatch, kPoll, kDevices, kUnknown } kind; nlohmann::json arg; };
std::optional<GpsdCommand> ParseGpsdCommand(std::string_view line);  // "?WATCH={...};" ; nullopt = malformed JSON
std::string ErrorJson(std::string_view message);
}
```
Each JSON string is one object terminated by `\r\n` (gpsd framing). Mapping:
TPV `mode` = 1 if !has_position, else 3; `status` = 3 RTK fixed, 4 RTK float, 2 DGNSS, 1 single;
`lat lon altHAE altMSL(=h_ell−geoid_sep) geoidSep`; `epx/epy/epv` = 1.96·σ (95 %) when sigmas exist;
`speed track` from vel when vel_valid; `dgpsAge dgpsSta` when diff_age_s > 0; `time` only from FixUnixNs.
GST: `lat lon alt` = σ_n σ_e σ_u.

- [ ] Tests: `GpsdJsonTest.TpvRtkFixed` (status 3, mode 3, altMSL), `GpsdJsonTest.TpvRtkFloat`, `GpsdJsonTest.NoTimeWithoutDate`, `GpsdJsonTest.IsoTimeMillis` (20261110, 43200.1 → `"2026-11-10T12:00:00.100Z"`), `GpsdJsonTest.EpFromSigmas`, `GpsdJsonTest.GstOmittedWithoutSigma`, `GpsdJsonTest.ParsesWatchEnable`, `GpsdJsonTest.ParsesBareCommands` (`?POLL;` `?VERSION;` `?DEVICES;`), `GpsdJsonTest.MalformedWatch` (nullopt), `GpsdJsonTest.EveryObjectEndsCrLf`.
- [ ] RED → implement → GREEN; commit.

### Task 3: Sky view (GSV + GSA) and the simulator emitting it

**Files:** Create `src/daemon/sky.{h,cc}`, `tests/daemon/sky_test.cc`. Modify `tests/sim/sim_lg290p.cc` (each epoch also sends one `GPGSA` with used PRNs and a 2-message `GPGSV` group), and update M1 tests only if counts change.

**Interfaces — Produces:**
```cpp
namespace lgrtk::daemon {
struct SkySat { uint16_t prn; int16_t el, az, ss; bool used; int gnssid; };
class SkyTracker {
 public:
  void FeedNmea(std::string_view line);     // GSV groups replace that talker's list when complete; GSA marks used PRNs
  std::vector<SkySat> Satellites() const;
  float Hdop() const; float Pdop() const;
};
std::string SkyJson(const SkyTracker&, const std::string& device, std::optional<int64_t> unix_ns);
}
```
`used` requires GSA PRN lists: the library `nmea::Gsa` has no PRNs, so `SkyTracker` parses the GSA
PRN fields itself (fields 3–14) after `nmea::ParseGsa` validated the checksum. gnssid: GPS 0, SBAS 1 (PRN 33–64 on GP), Galileo 2, BeiDou 3, QZSS 5, GLONASS 6, NavIC 7.

- [ ] Tests: `SkyTest.GroupCompletesOnLastMessage` (half group → previous list kept), `SkyTest.UsedFromGsa`, `SkyTest.TalkersKeptSeparately`, `SkyTest.SkyJsonCounts` (`nSat`, `uSat`).
- [ ] RED → implement → GREEN; full suite still green; commit.

### Task 4: Event loop and gpsd server

**Files:** Create `src/daemon/event_loop.{h,cc}` (poll set, eventfd wake, timers), `src/daemon/gpsd_server.{h,cc}`, `tests/daemon/gpsd_server_test.cc`.

**Interfaces — Produces:**
```cpp
namespace lgrtk::daemon {
class EventLoop {  // single-threaded; Post() is the only thread-safe call
 public:
  bool Init(std::string* err);
  void Add(int fd, short events, std::function<void(short revents)> cb);
  void Modify(int fd, short events); void Remove(int fd);
  void Post(std::function<void()> fn);       // any thread; runs on the loop
  void Every(int64_t period_ms, std::function<void()> fn);
  void Run(); void Quit();                    // Quit: any thread
};
int ListenTcp(const std::string& host, int port, std::string* err);   // SO_REUSEADDR, non-blocking
int ListenUnix(const std::string& path, std::string* err);           // unlinks a stale socket first
class ClientSet {  // shared by gpsd/raw/control servers: buffered non-blocking writes, cap, drop
 public:
  explicit ClientSet(EventLoop*, size_t cap_bytes = 256 * 1024);
  ...accept, per-client read callback(line-oriented), Send(id, data), Broadcast(pred, data), Count()
};
class GpsdServer {
 public:
  GpsdServer(EventLoop*, std::string device);
  bool Listen(const std::string& host, int port, std::string* err);
  void PublishFix(const Fix&);              // TPV (+GST) to json watchers; remembers latest for POLL
  void PublishNmea(std::string_view line);  // to nmea watchers
  void PublishSky(const std::string& sky_json);
  void SetActive(bool active, int baud);    // DEVICES to watchers on change; NoFixTpv on loss
  size_t Clients() const;
};
}
```
On connect: VERSION. `?WATCH={...}` → DEVICES + WATCH reply, then streaming per flags. `?POLL;` →
`{"class":"POLL","time":…,"active":n,"tpv":[…],"gst":[…],"sky":[…]}`. Malformed → `ERROR`. Lines over 4 KiB → client dropped.

- [ ] Tests (real TCP on 127.0.0.1 port 0, loop on a thread): `GpsdServerTest.VersionOnConnect`, `GpsdServerTest.WatchStreamsTpv`, `GpsdServerTest.NoStreamWithoutWatch`, `GpsdServerTest.PollReturnsLatest`, `GpsdServerTest.NmeaWatch`, `GpsdServerTest.CommandsSplitAcrossReads`, `GpsdServerTest.SlowClientIsDropped` (client never reads; publish > cap; other client still gets TPV), `GpsdServerTest.MalformedCommandGetsError`, `GpsdServerTest.LossSendsDevicesAndNoFix`, `EventLoopTest.PostFromOtherThread`.
- [ ] RED → implement → GREEN under ASan; commit.

### Task 5: Raw tap, chrony SOCK, systemd notify

**Files:** Create `src/daemon/raw_tap.{h,cc}`, `src/daemon/chrony.{h,cc}`, `src/daemon/sd_notify.{h,cc}`, tests `tests/daemon/{raw_tap,chrony,sd_notify}_test.cc`.

**Interfaces — Produces:**
```cpp
class RawTap { RawTap(EventLoop*); bool Listen(host, port, err); void Publish(std::span<const uint8_t>); };  // write-only clients, same cap/drop
struct ChronySample { timeval tv; double offset; int pulse; int leap; int pad; int magic; };  // magic 0x534f434b, sizeof == 40 on LP64
std::optional<ChronySample> MakeChronySample(const Fix&, int64_t arrival_realtime_ns);  // offset = fix_time − arrival; nullopt without date
class ChronySock { explicit ChronySock(std::string path); void Send(const ChronySample&); };  // AF_UNIX DGRAM, silent when chrony absent, retries each send
void SdNotify(std::string_view state);   // no-op without NOTIFY_SOCKET; supports abstract "@" sockets
int64_t WatchdogPeriodMs();               // WATCHDOG_USEC/2/1000, 0 when unset
```
Arrival realtime = `CLOCK_REALTIME now − (CLOCK_MONOTONIC now − t_arrival_ns)`, computed in the receiver callback.

- [ ] Tests: `RawTapTest.ForwardsBytesVerbatim` (incl. a binary RTCM frame), `RawTapTest.InputFromClientIgnored`, `ChronyTest.SampleLayout` (`sizeof`, magic), `ChronyTest.OffsetSign` (fix 12:00:00.000, arrival 12:00:00.080 → offset −0.080), `ChronyTest.NoSampleWithoutDate`, `ChronyTest.SendsToSocket` (bind a DGRAM socket, receive 40 bytes), `SdNotifyTest.SendsState` (bind DGRAM, setenv NOTIFY_SOCKET, receive `READY=1`), `SdNotifyTest.WatchdogPeriod`.
- [ ] RED → implement → GREEN; commit.

### Task 6: Control socket and `lgrtk ctl`

**Files:** Create `src/daemon/control.{h,cc}`, `tests/daemon/control_test.cc`. Modify `apps/lgrtk/main.cc` (subcommand `ctl [--socket PATH] status|reset KIND|apply-default`, default socket `/run/lgrtkd/control.sock`).

**Interfaces — Produces:**
```cpp
struct ControlHandlers { std::function<nlohmann::json()> status;
  std::function<nlohmann::json(const std::string& kind)> reset;   // hot|warm|cold|soft (factory refused)
  std::function<nlohmann::json()> apply_default; };
class ControlServer { ControlServer(EventLoop*, ControlHandlers); bool Listen(path, err); };
```
Protocol: one JSON object per line, `{"cmd":"status"}` → one line `{"ok":true,...}` or `{"ok":false,"error":"…"}`.
Socket mode 0660. The CLI client writes the line, reads one reply line (2 s timeout), prints it human-readable, exit 1 on `ok:false`, 1 when the socket is absent ("lgrtkd not running?").

- [ ] Tests: `ControlTest.StatusRoundTrip`, `ControlTest.UnknownCommand`, `ControlTest.FactoryResetRefused`, `ControlTest.BadJson`.
- [ ] RED → implement → GREEN; commit.

### Task 7: `lgrtkd` main and end-to-end test

**Files:** Create `apps/lgrtkd/main.cc`, `src/daemon/daemon.{h,cc}` (wires receiver + NTRIP + servers; testable without main), `tests/daemon_test.sh`. Modify `tests/CMakeLists.txt` (`DaemonTest.EndToEnd`).

`lgrtkd [--config PATH] [--check-config] [--foreground]` (default `/etc/lgrtk/lgrtkd.toml`; runs in the
foreground always — systemd supervises). Startup: load config (exit 2 on error), bind sockets (exit 1),
start receiver (+ NTRIP with `receiver` as sink and `SetPosition` from fixes), `SdNotify("READY=1")`,
watchdog timer, `STATUS=<state>` on every state change, SIGTERM/SIGINT via `signalfd` → orderly stop.
Logs go to stderr, one line per state change and per client connect/drop (journal picks them up).

- [ ] `daemon_test.sh <lgrtkd> <sim_tool> <lgrtk>`: start sim; write a temp TOML (ports from the
  free-port helper, control socket in the temp dir); start lgrtkd; a python3 one-shot client
  (`python3 -I -c`) reads VERSION, sends WATCH, gets ≥3 TPV with mode 3; raw tap yields bytes
  containing `$GNGGA`; `lgrtk ctl --socket … status` prints `streaming`; sim `unplug` (sim tool
  stdin command) → watcher sees DEVICES deactivation, then `replug` → TPV again within 5 s;
  SIGTERM → exit 0 within 2 s; `--check-config` on a bad file → exit 2.
- [ ] Add `unplug`/`replug` stdin commands to `tests/sim/sim_tool.cc` if absent.
- [ ] RED → implement → GREEN; commit.

### Task 8: System files, packaging, docs, release

**Files:** Create `packaging/99-lg290p.rules` (`1a86:55d3` → `/dev/lg290p0`, `ID_MM_DEVICE_IGNORE=1`, `GROUP="uucp"`, `TAG+="systemd"`), `packaging/lgrtkd.service` (`Type=notify`, `WatchdogSec=10`, `DynamicUser=no`, `User=lgrtk`, `SupplementaryGroups=uucp dialout`, `RuntimeDirectory=lgrtkd`, `LoadCredential=ntrip-password:/etc/lgrtk/ntrip-password`, hardening: `ProtectSystem=strict`, `ProtectHome=yes`, `NoNewPrivileges=yes`, `PrivateTmp=yes`, `DeviceAllow=char-ttyACM rw`, `RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6`), `packaging/lgrtk.sysusers`, `packaging/PKGBUILD`, `docs/daemon.md` (install, config reference, gpsd mapping table, chrony snippet `refclock SOCK /run/chrony.lgrtk.sock refid GNSS offset 0.0 delay 0.2`, raw tap with `str2str`, troubleshooting). Modify `CMakeLists.txt` (install lgrtkd, unit, rules, sysusers, example config to `share/lgrtk`), `README.md` (daemon section), `docs/decisions.md` (D23–D29), `docs/design.md` status line, `CHANGELOG.md` (v0.2.0), version 0.2.0 everywhere, CI (daemon deps + e2e).

Decisions to record: D23 deps toml++/nlohmann only for the daemon; D24 one event-loop thread, callbacks enqueue; D25 gpsd subset (VERSION/WATCH/POLL/DEVICES; TPV/GST/SKY; json+nmea watch only; ep* = 1.96σ); D26 chrony SOCK from NMEA time, off by default, offset to be calibrated (PPS later); D27 control socket JSON-lines + `lgrtk ctl` (full "CLI through the daemon" deferred); D28 sd_notify in-tree; D29 deferred to v0.3: NTRIP TLS, NTRIP server upload for an own base, PPS, `OnSatellites` in the library.

- [ ] `make check` green (format, tidy, guards, docs, tests); `systemd-analyze verify packaging/lgrtkd.service` if available; `makepkg --printsrcinfo` parses. Commit, PR, CI green, merge, tag `v0.2.0`, release notes.
