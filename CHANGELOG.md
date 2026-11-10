# Changelog

## Unreleased

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
- README hero photo of the module with the Q39 antenna; board diagram and pinout.

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
