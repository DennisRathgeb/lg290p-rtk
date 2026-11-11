# Changelog

## Unreleased

Fixes:
- `lgrtk probe`/`watch`/`ntrip run`: state used by receiver callbacks could be destroyed before
  the I/O thread stopped when start-up failed (use-after-free).
- NTRIP: chunk sizes are parsed strictly (`-1`, `0x1` and overlong sizes were accepted); the
  "check user name and password" message after HTTP 401 is no longer overwritten.
- `lg290p::PlanConfig` reports wanted values outside the documented range instead of dropping
  them silently.
- `lgrtk`: numbers are validated (`--baud 99999999999` and non-numeric message versions were
  silently truncated or read as 0); `ntrip sourcetable` rejects ports outside 1..65535.
- `lgrtkd`: `STOPPING=1` is sent to systemd once, not twice.
- `lgrtk ntrip run`: an empty password file is an error again (it connected without a
  password); `lgrtk probe` keeps the start-up GGA in its report.
- NTRIP behind a proxy (e.g. IQProxy) that answers `200`/`ICY 200 OK` at once and passes the
  caster's `401` on inside the stream: now `auth-failed` after one connection instead of a
  reconnect about once a second. Another HTTP error in the stream is a failure with backoff, and
  only RTCM frames reset the backoff.
- Socket paths starting with `@` are ordinary file names; only `$NOTIFY_SOCKET` uses abstract
  sockets (a configured `@` control socket would otherwise lose its file permissions).

Added:
- `lg290p::RestartByName`, `ToGgaQuality(FixType)`; `[[nodiscard]]` on factories and `Send()`.
- `tools/record-session.sh NAME [HOST MOUNT USER]`: host is an argument, the password file comes
  from `LGRTK_NTRIP_PASSWORD_FILE`, ports from `LGRTK_GPSD_PORT`/`LGRTK_RAW_PORT`, recordings go
  to `~/.local/share/lgrtk/recordings`; a busy port is reported and nothing is left running on
  exit.

Internal:
- Shared helpers (`UniqueFd`, `MonotonicNs`, `ParseNumber`, `MakeUnixAddress`, HTTP/NTRIP
  networking) replace per-file copies; every file descriptor is owned by RAII.
- The CLI is split into one file per command group; it links a small control-client library
  instead of the whole daemon.
- `NtripClient::Session` split into request, response header and stream steps.
- Stricter clang-tidy (bug-prone, performance, rule of five) and grouped includes, enforced in CI.
- Documentation: `docs/architecture.md` replaces the design notes and milestone plans; README
  rewritten.

## v0.2.0 (2026-11-10)

The daemon: the receiver for every program on the machine.

- `lgrtkd`: one receiver plus optional NTRIP, served over the **gpsd JSON protocol** (VERSION,
  WATCH, POLL, DEVICES; TPV with RTK fixed/float status, GST, SKY), a **raw byte tap**, a
  **chrony SOCK** refclock and a **control socket**. One event-loop thread; slow clients are
  dropped, never waited for. Clients stay connected across unplugging.
- `lgrtk ctl status | reset KIND | apply-default` talks to a running daemon.
- TOML configuration with strict keys (`lgrtkd --check-config`), NTRIP password from a file or a
  systemd credential.
- systemd unit (`Type=notify`, watchdog, hardening), udev rule (`/dev/lg290p0`, ModemManager off),
  sysusers file, PKGBUILD, CMake install of all of them.
- Simulator sends GSA/GSV; end-to-end daemon test (gpsd client, raw tap, ctl, unplug/replug,
  SIGTERM) in CI.
- `tools/record-session.sh`: record a session (raw bytes, gpsd reports, status) through lgrtkd.
- `docs/hardware.md`: board diagram and pinout, outdoor measurements (10 Hz, up to 22
  satellites, GST with 3 decimals on firmware A06S).

## v0.1.0 (2026-11-10)

First release: library, CLI, NTRIP, projection.

- `StreamFramer`: NMEA + RTCM3 on one stream, checksum and CRC-24Q, resync after noise.
- `nmea`: GGA, RMC, GST, GSA, GSV, VTG, ZDA, `$PQTMEPE`, `$PQTMVERNO`, `$PQTMSVINSTATUS`, command replies.
- `EpochAssembler` and `Fix`: one record per epoch, closes on the learnt sentence pattern.
- `lg290p`: typed `$PQTM` command builders (checked against the protocol spec v1.1),
  read-back, diff-based configuration planner (save/reset only when needed).
- `Lg290pReceiver`: state machine (identify with baud detection, configure, streaming, stale,
  reconnecting, fault), RTCM injection without splitting frames, command queue, `RawRecorder`.
  Discards the stale bytes the CH343 bridge replays after open (found on the bench).
- `NtripClient`: NTRIP v1/v2, chunked transfer, GGA upload, backoff, stall detection, stops on 401.
- `Projection` (PROJ, refuses ballpark transformations and names missing grids) and `LocalFrame`.
- `lgrtk` CLI: probe, watch, status, config, reset, base, ntrip, project.
- `FixProjector` (projection chosen by the fix's frame; undeclared frames refused) and
  `Lg290pReceiver::Snapshot()` (fix, age and state under one lock).
- Review fixes before release: the framer resynchronises at a `$` inside a truncated sentence
  and after over-long lines; settings that cannot be read back are left unchanged (no blind
  writes, no flash wear); SBAS fixes and differential fixes without our RTCM are tagged
  autonomous; the stale threshold follows the epoch rate; command writes never block the I/O
  thread; `NtripClient::Stop()` is fast while connecting; `Send()` before `Start()` resolves.
- CMake package (`find_package(lgrtk)`), pkg-config file, examples, Doxygen docs.
