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
