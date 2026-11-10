# Design decisions

The choices that shape `lgrtk`, each with its reason and the condition under which it should be
revisited. Numbers are stable; code and docs refer to them (e.g. "D6").

| # | Decision | Why | Revisit if |
|---|---|---|---|
| D1 | Userspace library + daemon, no kernel driver | `cdc-acm` already binds the CH343; parsing, NTRIP and PROJ do not belong in the kernel | never |
| D2 | System-wide output speaks the gpsd JSON protocol | every existing GNSS client works unchanged | a consumer needs data gpsd JSON cannot carry |
| D3 | Licence MIT | simplest permissive licence; compatible with PROJ (MIT) | a patent grant matters → Apache-2.0 |
| D4 | Names: repository `lg290p-rtk`, library/namespace/CLI `lgrtk`, daemon `lgrtkd` | short, module-specific, no collision with gpsd or RTKLIB names | — |
| D6 | One owner per serial port (`TIOCEXCL`): either an application linking the library, or `lgrtkd` | a second process interleaving commands breaks configuration | several programs must share the port without the daemon |
| D7 | Every fix is tagged autonomous or corrected; the CRS of each is declared by the user, never guessed | the receiver cannot know the frame of its corrections (e.g. CHTRS95 vs ITRF ≈ 0.8 m) | — |
| D8 | Base mode = survey-in/fixed position + RTCM output; uploading to a caster comes later | delivering corrections to rovers needs a second receiver to test | — |
| D10 | Both a PROJ `Projection` (any CRS, e.g. LV95 + LHN95) and a PROJ-free tangent-plane `LocalFrame`; the application picks | grid coordinates and tangent-plane ENU differ by the meridian convergence | — |
| D11 | Linux only, C++20, gcc 11.4 floor, Google C++ style | embedded (Jetson) toolchains | — |
| D12 | The library has no dependency beyond the C++ standard library (PROJ optional, D13) | easy to vendor, easy to package | — |
| D13 | PROJ is an optional component (`LGRTK_WITH_PROJ`) | users who only want fixes need not install PROJ | — |
| D14 | Pictures: own photos and SVG diagrams; vendor photos only linked | vendor photos are copyrighted | — |
| D15 | Firmware flashing out of scope | Quectel's flasher is Windows-only (QGNSS) | a Linux flasher protocol is documented |
| D17 | Raw-stream recording belongs to `lgrtk` (`RawRecorder` on the `OnRaw` tap; the daemon's raw tap) | every RTK application wants replayable captures | — |
| D18 | Discard the first 150 ms after every open | the CH343 replays stale FIFO bytes (seen: an hour-old epoch) on open | the bridge is replaced |
| D19 | A setting whose current value cannot be read is not written (reported in `last_error`) | blind writes would save to flash, and maybe reset, on every start | a setting must be forced: add an explicit option |
| D20 | `FixFrame::kCorrected` requires RTCM injected through `lgrtk` within the correction age + 5 s; SBAS DGNSS (station 120..158) is autonomous | otherwise SBAS/PPP fixes would be projected with the correction CRS (~0.8 m) | the receiver reports the correction source explicitly |
| D21 | Stale threshold: 3 epoch periods (read-back fix rate, then measured), at least 500 ms | a fixed 500 ms flaps at 1 Hz | — |
| D23 | The daemon's TOML and JSON libraries (toml++ 3.4, nlohmann/json 3.11) link only into `lgrtkd`; system packages first, else fetched with a pinned hash | keeps D12 | — |
| D24 | One event-loop thread (`poll`) serves every socket; receiver and NTRIP callbacks only post work to it; per-client output capped at 256 KiB, the client is dropped on overflow; at most 64 clients per socket; accepting pauses while out of file descriptors | no locks around sockets; a stuck client cannot slow the receiver | — |
| D25 | gpsd subset: VERSION, WATCH (`enable`/`json`/`nmea`), POLL, DEVICES; reports TPV, GST, SKY, ERROR; `epx/epy/epv` = 1.96 σ; `status` 3 RTK fixed, 4 RTK float | what cgps, QGIS and Python `gps` use; raw/binary watch and `?DEVICE=` would bypass the configuration logic | a client needs another report |
| D26 | chrony SOCK sample from the NMEA time against the arrival time, off by default, sent whatever the offset (chrony's `makestep` decides); documented as needing calibration | NMEA time has tens of ms latency; PPS is the precise path; a ±0.5 s gate would never set a clock without RTC | — |
| D27 | Control socket: one JSON object per line (`status`, `reset`, `apply-default`) and `lgrtk ctl`; factory reset refused there; `apply-default` restarts the receiver object | small, scriptable, enough for operation | every CLI command should work through the daemon |
| D28 | systemd notify (READY, STATUS, WATCHDOG, STOPPING) implemented in-tree, no libsystemd | 50 lines instead of a dependency | — |
| D30 | The daemon runs as system user `lgrtk` (sysusers; member of `uucp` or `dialout`, picked at configure time) in a hardened unit; the NTRIP password comes through `ImportCredential=` from `/etc/credstore` (optional, unlike `LoadCredential=`); serial drivers are loaded before start | no root, no password in the config file; `DeviceAllow=` needs the device group to exist at start | — |

Gaps in the numbering are decisions that only concerned how the project was set up.
