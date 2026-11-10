# lg290p-rtk: an open-source Linux toolkit for the Quectel LG290P RTK receiver

Design, 2026-11-10. Status: draft for Dennis's approval.
Written in `ar-stack` because the conversation happened here; it moves to the new repository's
`docs/design.md` when that repository is created.

## 1. Intent

Dennis wants the LG290P (on the Waveshare "LG290P GNSS RTK Module", USB-C, CH343 bridge) to be a
dependable RTK position source that "just works": it configures the receiver, exposes its important
functions, hands fixes to other software fast, notices and reports when the connection or the data
is lost, and reconnects by itself. Corrections (NTRIP) plug in as soon as the caster login arrives.
Projection into a site or national grid must be fast and reusable.

It must not be AR-specific. Dennis wants to reuse it in any future RTK project, either by linking
the library or by installing it on a Linux machine and having the functionality system-wide, and
he wants it publishable for other people who own the module. Therefore: **its own public GitHub
repository under Dennis's personal account (`DennisRathgeb`) from day one**, open-source licence,
a README that teaches the module itself (with pictures), and `ar-stack` consumes it as a dependency.

Why not existing software (researched 2026-11-10, see §10): there is no Linux/C++/ROS driver for
the LG290P; gpsd treats it as generic NMEA and cannot configure it (`$PQTM*`); SparkFun's library is
Arduino-only; the C++ NTRIP options are ROS-bound or heavy GUI tools (BNC); RTKLIB is monolithic C
not built to share a real-time loop. What is reused: **PROJ** + swisstopo grids for projection, the
**gpsd JSON protocol** as the system-wide output (so existing clients work unchanged), SparkFun's
library as the readable catalogue of command strings and orderings, the Quectel protocol
specification v1.1 as the authority.

## 2. Goals

1. **Receiver driver with an explicit state machine** (§4) that owns the serial port, survives
   unplug/replug and receiver resets without a process restart, and reports every state change.
2. **Full command layer** for what matters on this receiver: read/write a setting with
   acknowledgement checking, save to NVM, resets (hot/warm/cold/software), firmware/serial query,
   fix rate, per-message output rates, rover/base mode, RTK mode and max differential age,
   constellations, elevation and C/N0 masks, NMEA decimal places, PPS, survey-in / fixed base
   position, raw observation (RTCM MSM) output.
3. **Fast data path:** one normalised `Fix` per epoch (10 Hz default, 20 Hz max) delivered by
   callback with sub-millisecond parse latency and an allocation-free hot path, plus a lock-free
   "latest fix" snapshot for pollers.
4. **NTRIP client** (v1 and v2) that streams corrections into the receiver, sends GGA back for VRS
   mountpoints, reconnects with backoff, and reports correction age and state.
5. **Projection** as its own reusable class: geodetic → any target CRS (and optionally local ENU
   around an origin), transform built once, microseconds per point, offline grids.
6. **System integration:** daemon (`lgrtkd`) speaking the gpsd protocol, raw stream tap, chrony
   time source; CLI (`lgrtk`); udev rule; systemd unit; config file; Arch package and `make install`.
7. **Publishable quality:** permissive licence, CI with no hardware, versioned API, documentation
   that teaches the module, examples.

## 3. Non-goals and anti-goals

Non-goals (v1): a kernel driver (the kernel part already exists: `cdc-acm` binds the CH343 as
`/dev/ttyACM*`; protocol logic belongs in userspace); a GUI; firmware flashing (Quectel's flasher is
Windows-only QGNSS); Windows/macOS; sensor fusion; other receivers (but see §5.1: the receiver layer
sits behind an interface so a second model can be added later without touching NTRIP/projection).

Anti-goals, things that would make it a failure even if it "works":
- **Stale data presented as fresh.** Every fix carries its age and the driver's state; consumers can
  never mistake the last fix before an unplug for a live one.
- **Writing the receiver's NVM on every start.** Configuration is compared against a read-back and
  only differences are written; `SAVEPAR` + reset only when something changed.
- **Blocking or allocating in the hot path**, which would make it unusable inside `ar-core`'s
  real-time producer threads.
- **Frame confusion.** Positions are tagged with the frame they are in (it depends on the correction
  source: CHTRS95 with swipos, ~ITRF current epoch when autonomous; ~0.8 m apart in Switzerland).
- A README that only lists API calls; it must make someone new to the module successful.

## 4. Receiver state machine

```
            ┌────────────── port vanished / I/O error ───────────────┐
            ▼                                                         │
 kDisconnected ──port present──► kOpening ──opened──► kIdentifying ──firmware reply──► kConfiguring
      ▲  ▲                          │ fail                 │ no reply in 2 s:                 │ all
      │  └────── backoff ◄──────────┘                      │ try next baud                    │ acked
      │                                                    ▼                                  ▼
      │                                          (baud list exhausted) ──► kFault      kStreaming
      │                                                                                  │  ▲
      └──────────── kReconnecting ◄── port vanished / I/O error ──── kStale ◄─ no epoch ─┘  │
                         │                                            │   in N epochs       │
                         └── backoff, then kOpening                   └── data resumes ─────┘
```

- **Opening:** raw 8N1, `VMIN 0 / VTIME 1`, exclusive (`TIOCEXCL`) so ModemManager or a second
  process cannot interleave.
- **Identifying:** `$PQTMVERNO` → firmware string, also `PQTMUNIQID`. Baud detection order: the
  configured baud, then 460800 (factory), 921600, 115200, 230400, 9600. On the CH343 the host baud
  is forwarded to the module's UART1, so a mismatch shows as garbage/no reply.
- **Configuring:** read back each managed setting, write differences, check `OK`/`ERROR,<code>`
  replies (timeout and bounded retries per command), `SAVEPAR` + `SRR` only if a
  reset-requiring setting changed, then re-identify. A receiver that keeps NAKing a command → `kFault`
  with the command and error code in the status, never a silent partial configuration.
- **Streaming:** epochs arrive. **Stale:** no complete epoch for `stale_after` (default 3 expected
  epoch periods, at least 500 ms); the port is still open. **Reconnecting:** port gone or I/O error;
  backoff 250 ms doubling to 5 s; also reacts immediately to the device reappearing (inotify on
  `/dev` or udev monitor; builder's choice).
- Every transition produces a `StatusEvent {old, new, reason, t}`; `Status()` gives a snapshot:
  state, firmware, port, baud, counters (bytes, NMEA/RTCM frames, checksum errors, epochs, overruns,
  reconnects), last-epoch age, last error.
- Unsolicited receiver resets (detected by a `$PQTMTXT`/boot banner or UTC jumping back) re-enter
  `kIdentifying` so configuration that did not persist is re-applied.

NTRIP has its own small machine: `kIdle → kConnecting → kStreaming ⇄ kStalled (no data for 10 s) →
kBackoff → kConnecting`, plus `kAuthFailed` (HTTP 401: stop, do not hammer the caster) and
`kNoPosition` (VRS mountpoint and no fix yet to send as GGA).

## 5. Architecture

### 5.1 Library `liblgrtk` (C++20, namespace `lgrtk`)

| Component | Responsibility |
|---|---|
| `SerialPort` | open/configure/read/write a tty; non-blocking writes; exclusive lock |
| `StreamFramer` | splits the byte stream into NMEA sentences (`$…*CS\r\n`, checksum checked) and RTCM3 frames (`0xD3`, 10-bit length, CRC-24Q); resynchronises after garbage; counts errors |
| `Nmea` parsers | GGA, RMC, GST, GSA, GSV, ZDA, VTG + Quectel `PQTMEPE`, `PQTMPVT`, `PQTMDOP`, `PQTMPL`, `PQTMVERNO`, `PQTMSVINSTATUS`, command replies; zero allocation |
| `EpochAssembler` | groups the sentences of one UTC epoch into one `Fix` (GGA + GST/EPE + RMC velocity + ZDA date), emits when the epoch closes |
| `Lg290p` | the command layer of §2.2 as typed methods (`SetFixRate(100ms)`, `SetMessageRate(Msg::kGst, 1)`, `SetRtk(...)`, `SurveyIn(...)`…), builds checksummed strings, matches replies |
| `Receiver` (interface) / `Lg290pReceiver` | the state machine of §4 on its own I/O thread; callbacks `OnFix`, `OnStatus`, `OnSatellites`, `OnRtcmOut` (raw obs / base output), `OnRaw` (byte tap); `LatestFix()` lock-free snapshot; `Inject(bytes)` for corrections |
| `NtripClient` | NTRIP v1/v2 (HTTP, chunked), optional TLS, sourcetable fetch, GGA upload interval, backoff; writes into any `CorrectionSink` (the receiver) |
| `Projection` | wraps PROJ: `Projection::Create(from_crs, to_crs, options)` built once; `Forward(lat, lon, h) → (x, y, z)` in µs; optional `LocalFrame` (ENU around an origin); grid availability checked at creation with a clear error |

`Fix` (the public record): UTC time and date, GPS week/TOW when known, host monotonic arrival time
(stamped right after the read that completed the epoch, before parsing), lat/lon (degrees, full
precision), ellipsoidal height and the receiver's geoid separation, fix type (none/single/DGNSS/
RTK float/RTK fixed/PPP/dead reckoning), satellites used, HDOP/PDOP, per-axis 1-σ (from GST, else
PQTMEPE, flagged which), velocity ENU when valid, differential age and reference station id, the
frame tag, and the driver state at emission.

Threading: one I/O thread per receiver (read with 100 ms timeout, so `Stop()` joins within ~1 s),
corrections written between reads with a per-iteration byte cap (4 KiB) so they never starve
reading. Callbacks run on the I/O thread; the library documents that they must be short. After
`Start()` the hot path (read → frame → parse → assemble → callback) does not allocate.

### 5.2 Daemon `lgrtkd`

Uses the library; one receiver, optional NTRIP. Outputs:
- **gpsd JSON protocol** on TCP 127.0.0.1:2947 (configurable): `VERSION`, `DEVICES`, `WATCH`, `POLL`,
  and reports `TPV` (incl. `mode`, `status` mapped from RTK fixed/float, `epx/epy/epv` from GST,
  `dgpsAge`, `dgpsSta`), `SKY`, `GST`. So `cgps`, `gpsmon`, QGIS, Python `gps`, OpenCPN work unchanged.
  Port 2947 conflicts with a running gpsd; documented, configurable.
- **Raw tap:** TCP port that streams the untouched receiver bytes (NMEA + RTCM) for loggers, RTKLIB
  `str2str`, post-processing.
- **chrony:** SOCK refclock (`/run/chrony.lgrtk.sock`) with the NMEA time; PPS later.
- **Status/control:** a Unix socket for `lgrtk` (state, counters, apply config, reset, base mode).
- systemd unit (`Type=notify`, watchdog), logs to the journal.

### 5.3 CLI `lgrtk`

`lgrtk status`, `lgrtk probe` (today's `arstack gnss-probe`: sentences, rates, quality, GST present,
firmware, UTC monotonicity, `--raw` capture), `lgrtk config show|apply`, `lgrtk reset hot|warm|cold`,
`lgrtk base survey-in|fixed`, `lgrtk ntrip sourcetable`, `lgrtk watch` (live fix line). Works either
through the daemon or directly on the port when no daemon runs.

### 5.4 System files

udev rule (`1a86:55d3` → `/dev/lg290p0` symlink, `ID_MM_DEVICE_IGNORE=1`, group `uucp`/`dialout`),
`/etc/lgrtk/lgrtk.toml` (port, baud, receiver settings, NTRIP caster/mountpoint, credentials via a
separate root-only file or systemd credential, outputs), systemd unit, `PKGBUILD`, CMake `install`
and an exported CMake package (`find_package(lgrtk)`), pkg-config file.

## 6. Constraints

- Linux, C++20, CMake. Must build with gcc 11.4 (the Jetson's floor, same as `ar-core`) and on
  x86 Arch/Ubuntu; aarch64 Jetson is a target.
- Dependencies kept small: PROJ (only for `Projection`; optional component), OpenSSL (only for NTRIP
  over TLS; optional), a small JSON and TOML library (header-only, vendored or fetched). No Boost,
  no ROS.
- Licence **MIT** (my call: simplest permissive licence, compatible with PROJ (MIT) and with
  `ar-stack`; Apache-2.0 is the alternative if a patent grant matters).
- Google C++ style like `ar-core` (`docs/process/cpp-style.md`) so the code looks the same in both repos.
- Configuration changes must be idempotent and minimal (anti-goal: NVM wear).
- Credentials never logged and never in the repository.
- Pictures: Waveshare's and Quectel's product photos are copyrighted, so the README uses **our own
  photos of the module** (Dennis takes them at the bench: board top, USB-C + antenna connected,
  pinout close-up) plus SVG diagrams we draw (wiring, state machine, data flow), and links to the
  vendor pages. Placeholders until the photos exist.

## 7. Tests and verification

- Unit tests with no hardware: framer (partial reads, garbage, interleaved NMEA/RTCM, CRC errors),
  parsers (spec examples and captured lines), epoch assembler (GST before/after GGA, missing GST),
  command builder/reply matcher, projection (origin → 0; 1 m east → 1 m; swisstopo REFRAME
  reference points from `research/notes/ntrip-and-swiss-coordinates.md`).
- Integration tests on a pseudo-terminal with a **simulated LG290P** (replays a capture, answers
  `$PQTM` commands, can be "unplugged"): state machine through every transition, baud detection,
  reconnect, NAK → fault. A local fake NTRIP caster for the client (200, 401, chunked, drop).
- Captures from the real module (starting with today's
  `~/data/bench/2026-11-10-lg290p/probe1.nmea`, firmware `LG290P03AANR01A06S`) become test fixtures.
- CI (GitHub Actions): build + tests on Ubuntu 22.04 (gcc 11) and 24.04, clang-tidy, ASan/UBSan.
- Bench acceptance (Dennis, later, with sky view): `lgrtk probe` matches today's probe; unplug and
  replug recovers to `kStreaming` within 2 s with no restart; `cgps` shows fixes through `lgrtkd`;
  with the caster login, RTK float then fixed and a correction-age display; 20 Hz for 10 min with no
  checksum errors or overruns.

## 8. Integration into ar-stack

- `ar-stack` pulls `lgrtk` with CMake `FetchContent` pinned to a tag (or `find_package` when
  installed). `arcore::GnssSource` becomes a thin adapter: `Lg290pReceiver` + `NtripClient` +
  `Projection` (site origin from `Config::Site::origin`), maps `lgrtk::Fix` to the frozen
  `arcore::GnssFix` (112 bytes; no layout change) and `Status()` to `DeviceStatus`.
- This supersedes tasks 1–4 of `docs/superpowers/plans/2026-11-10-device-sources.md` (SiteFrame,
  LineReader, EpochAssembler, GnssSource thread) and replaces `code/core/src/devices/gnss/nmea.cc`;
  VITURE tasks stay. An ADR records the decision (external library, licence, ownership).
- `arstack gnss-probe` stays until `lgrtk probe` is released, then the bench page points to it.

## 9. Decided vs left to the builder

Decided by Dennis: own repository under his personal GitHub account from the start; open-source
licence; README that features this module with pictures; library usable alone *and* installable
system-wide; reusable for any RTK localisation project; reconnect and loss reporting; NTRIP once
the login exists; fast, reusable projection; reuse existing software where good.

My calls (Dennis to confirm while reading): userspace instead of a kernel driver; gpsd protocol as
the system-wide output; MIT; C++20; names `lg290p-rtk` (repo), `lgrtk` (library, namespace, CLI),
`lgrtkd` (daemon); Linux only; base-station mode included (an own base on site is the planned
production answer, see Q06); firmware flashing out of scope; TOML config.

Left to the builder: internal class layout below the components of §5.1, JSON/TOML library choice,
hotplug detection mechanism, exact backoff constants within the stated bounds, test framework
(GoogleTest like `ar-core` recommended), packaging details.

### 9.1 Builder-check resolutions (my calls, 2026-11-10; Dennis asked me to proceed without questions)

1. **Port ownership: exactly one owner per port.** The library opens the port itself; `lgrtkd` is
   one possible owner, an application linking the library (ar-core) is another, never both. On the
   Jetson ar-core links the library directly (lowest latency, full `Fix`); the daemon is for
   machines where system-wide access matters more. A native rich client protocol for the daemon
   (so ar-core could be a daemon client) is a later option, not v1. The library exposes a raw-byte
   tap callback so the owner can still log the stream.
2. **Frame tag:** `Fix::frame` is `kAutonomous` (no differential corrections in use: fix type
   single, or differential age absent) or `kCorrected`. The CRS of corrected positions is
   **declared in the configuration** per correction source (`corrections.crs`, e.g. `EPSG:4936`
   ETRS89/CHTRS95 for swipos, or whatever the own base was entered in); the autonomous CRS is
   declared as well (default `EPSG:4979`, WGS 84 ≈ ITRF current epoch). The library does not guess;
   `Projection` is built for an explicit source CRS and the receiver wrapper offers
   `ProjectFix()` that picks the projection matching the fix's tag, so mixed frames cannot happen
   silently. Unknown/undeclared → projection refused with a reason.
3. **Base mode in v1** = configure survey-in or fixed position, read `PQTMSVINSTATUS`, and receive
   the base's RTCM output through `OnRtcm` and the raw tap. Delivering it to rovers (NTRIP server
   upload / caster) is v1.1. No two-module acceptance test in v1.
4. **Order:** library-first. Milestone 1 (tag `v0.1.0`): framer, parsers, epoch assembler, command
   layer, state machine, simulated receiver, `Projection`/`LocalFrame`, NTRIP client, `lgrtk`
   CLI (probe/status/watch/config/reset). Milestone 2 (`v0.2.0`): `lgrtkd` with gpsd JSON, raw tap,
   chrony SOCK, udev/systemd/PKGBUILD. ar-stack can pull `v0.1.0` as soon as it exists.
5. **Site frame:** the library offers both — `Projection` to any CRS (e.g. compound
   `EPSG:2056+5729`, LV95 + LHN95 heights, geoid applied by PROJ's grid) and `LocalFrame`, a true
   geodetic tangent-plane ENU around an origin. It does not decide which one a project uses. For
   ar-stack, ADR 0002 decides; the adapter is an ar-stack change and gets its own issue.
6. **Names and licence as in §9**; the repository is created **private** under `DennisRathgeb`
   so Dennis flips it public after his review (making it public cannot be undone in practice).

## 10. Sources

- Existing research (verified against the protocol spec v1.1, Waveshare wiki/demo, SparkFun
  library source): `research/notes/rtk-gnss-lg290p.md`, `research/notes/ntrip-and-swiss-coordinates.md`,
  `docs/hardware/rtk-lg290p.md`.
- Bench, 2026-11-10: CH343 `1a86:55d3` → `/dev/ttyACM0`, default baud works, firmware
  `LG290P03AANR01A06S` (2025/09/18), default output GGA/RMC/GLL/VTG 10 Hz, GSA, GSV, no GST.
- Existing-software search, 2026-11-10: SparkFun LG290P docs
  (https://docs.sparkfun.com/SparkFun_LG290P_Quadband_GNSS_RTK_Breakout/print_view/), ROS
  `ntrip_client` (https://index.ros.org/p/ntrip_client/), `ublox_dgnss` NTRIP node, BNC
  (https://igs.bkg.bund.de/ntrip/bnc), SparkFun Extensible Message Parser (Arduino only, PQTM since
  v2.0.2), minmea/libnmea (C NMEA only, no PQTM, no RTCM).
