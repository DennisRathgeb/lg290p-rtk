# Decisions taken without Dennis (2026-11-10)

Dennis asked for autonomous work ("no more questions … keep track of decisions you made"). Every
call below is reversible; each says why. Decisions he made himself are in `design.md` §9.

| # | Decision | Why | Revisit if |
|---|---|---|---|
| D1 | Userspace library + daemon, no kernel driver | `cdc-acm` already binds the CH343; parsing, NTRIP and PROJ do not belong in the kernel | never |
| D2 | System-wide output speaks the gpsd JSON protocol (M2) | every existing GNSS client works unchanged | a consumer needs data gpsd JSON cannot carry |
| D3 | Licence MIT | simplest permissive licence; compatible with PROJ (MIT) | a patent grant matters → Apache-2.0 |
| D4 | Names: repo `lg290p-rtk`, library/namespace/CLI `lgrtk`, daemon `lgrtkd` | short, module-specific, no collision with gpsd/rtklib names | before the repo is public |
| D5 | Repository created **private** under `DennisRathgeb` | making it public cannot be undone in practice; Dennis flips it after review | Dennis reviewed it |
| D6 | One owner per serial port (`TIOCEXCL`); ar-core links the library, the daemon is the alternative owner | a second process interleaving commands breaks configuration | ar-core must share the receiver with chrony/loggers on the Jetson |
| D7 | Fix frame tag = autonomous vs corrected; CRS of each declared in config, never guessed | the receiver cannot know the frame of its corrections (CHTRS95 vs ITRF ≈ 0.8 m) | — |
| D8 | Base mode v1 = survey-in/fixed + RTCM out; NTRIP server upload in v1.1 | only one module exists today; delivery to rovers needs a second one to test | second module arrives |
| D9 | Library-first milestones: M1 library + CLI (`v0.1.0`), M2 daemon + system files | ar-stack can integrate M1 immediately | — |
| D10 | Library offers both PROJ `Projection` (any CRS, e.g. LV95+LHN95) and a PROJ-free tangent-plane `LocalFrame`; the consuming project picks | LV95 grid offsets and tangent-plane ENU differ by meridian convergence (~1° at Nalps) | — |
| D11 | Linux only, C++20, gcc 11.4 floor, Google C++ style (same as ar-core) | Jetson toolchain; one style across both repos | — |
| D12 | M1 has no JSON/TOML dependency; CLI takes flags; TOML config arrives with the daemon | fewer dependencies for the library users | — |
| D13 | PROJ is an optional component (`LGRTK_WITH_PROJ`) | users who only want fixes need not install PROJ | — |
| D14 | Pictures: own photos of the module (placeholders until Dennis takes them) + our SVG diagrams; vendor photos only linked | Waveshare/Quectel photos are copyrighted | — |
| D15 | Firmware flashing out of scope | Quectel's flasher is Windows-only (QGNSS) | a Linux flasher protocol is documented |
| D16 | On this laptop PROJ 9.9.0 was built from source into `~/.local` | `sudo` needed a password; Arch `proj` package is the clean alternative (`pacman -S proj`) | — |
| D17 | Raw-stream recording lives in lgrtk (library `RawRecorder` on the `OnRaw` tap; daemon raw tap), not in ar-core | Dennis on issue #64: recording the raw stream is the RTK toolkit's job | — |
| D18 | Discard the first 150 ms after every open | bench 2026-11-10: the CH343 replays stale FIFO bytes (an hour-old epoch) on open | the bridge is replaced |
| D19 | A setting whose current value cannot be read is not written (reported in `last_error`) | blind writes would save to flash and maybe reset on every start (final review) | a setting must be forced: add an explicit option |
| D20 | `FixFrame::kCorrected` requires RTCM injected by us within the correction age + 5 s, and SBAS DGNSS (station 120..158) is autonomous | otherwise SBAS/PPP fixes would be projected with the correction CRS (~0.8 m) | the receiver reports the correction source explicitly |
| D21 | Stale threshold automatic: 3 epoch periods (read-back FIXRATE, then measured), at least 500 ms | a fixed 500 ms flaps at 1 Hz | — |
| D22 | Deferred to v0.2.0: `OnSatellites` (GSV is parsed, not surfaced), `PQTMUNIQID` at identify, GPS week/TOW in `Fix` (needs leap seconds), an overrun counter | spec items left out of the M1 plan; none blocks ar-stack | ar-stack needs one of them |
| D23 | The daemon's TOML and JSON libraries (toml++ 3.4, nlohmann/json 3.11) link only into `lgrtkd`; system packages first, else fetched with a pinned hash | the library stays dependency-free (D12) | — |
| D24 | One event-loop thread (`poll`) serves every socket; receiver and NTRIP callbacks only post work to it; per-client output capped at 256 KiB, the client is dropped on overflow; at most 64 clients per socket, accepting pauses while out of file descriptors | no locks around sockets; a stuck client cannot slow the receiver | — |
| D25 | gpsd subset: VERSION, WATCH (`enable`/`json`/`nmea`), POLL, DEVICES; reports TPV, GST, SKY, ERROR; `epx/epy/epv` = 1.96 σ; `status` 3 RTK fixed, 4 RTK float | what cgps, QGIS and Python `gps` use; raw/binary watch and `?DEVICE=` would bypass the configuration logic | a client needs another report |
| D26 | chrony SOCK sample from the NMEA time against the arrival time, off by default, sent whatever the offset (chrony's `makestep` decides; first version gated at ±0.5 s, dropped after the final review: it would never set a clock without RTC); documented as needing calibration | NMEA time has tens of ms latency; PPS is the precise path (later) | — |
| D27 | Control socket: one JSON object per line (`status`, `reset`, `apply-default`) and `lgrtk ctl`; factory reset refused there; `apply-default` restarts the receiver object | the full "every CLI command through the daemon" is larger than M2 needs | Dennis wants `config set` through the daemon |
| D28 | systemd notify (READY, STATUS, WATCHDOG, STOPPING) written in-tree, no libsystemd | 50 lines instead of a dependency | — |
| D29 | Deferred to v0.3: NTRIP over TLS, NTRIP server upload for an own base, PPS time, `OnSatellites` in the library (the daemon tracks GSV itself), `config set` through the daemon | not needed for the bench test with the caster login | the caster requires TLS |
| D30 | The daemon runs as system user `lgrtk` (sysusers, member of `uucp`/`dialout` picked at configure time), hardened unit, NTRIP password through `ImportCredential=` from `/etc/credstore` (optional; `LoadCredential=` fails the unit when the file is missing), serial drivers loaded before start | no root, no password in the config file; `DeviceAllow=` needs the device group to exist at start | — |
