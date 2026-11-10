# The Quectel LG290P and the Waveshare board: a practical guide

Everything here was checked against the Quectel *LG290P(03) & LGx80P(03) GNSS Protocol
Specification v1.1* (2025-07-25), the Waveshare wiki and demo code, SparkFun's documentation and
library, or our own bench (marked **bench**, with date and firmware). Claims from forums are
marked *reported*.

## The receiver

| | LG290P (03) |
|---|---|
| Constellations | GPS, GLONASS, Galileo, BeiDou, QZSS, NavIC, SBAS (all on by default) |
| Signals | L1/L2/L5 families plus Galileo E6, BDS B3I; 1040 channels |
| Accuracy | autonomous 0.7 m CEP; **RTK 0.8 cm + 1 ppm** horizontal, 1.5 cm + 1 ppm vertical (open sky, short baseline) |
| Rate | 10 Hz default, 20 Hz max (also in RTK); base mode fixed at 1 Hz |
| Time to fix | cold 28 s, hot 1.7 s; RTK convergence "5 s" (Quectel, open sky) |
| Interfaces | 3 UARTs, 9600 – 3 000 000 baud, default 460800 8N1; NMEA 0183 + RTCM 3 in/out on every UART |
| Power | 3.3 V, 91 mA |
| What it cannot do | heading from one antenna (that is the dual-antenna LG580P); PPP (Galileo HAS) only on newer firmware, ~15 min convergence (*reported*) |

## The Waveshare LG290P GNSS RTK Module

<!-- Photos: docs/images/board-top.jpg, docs/images/board-antenna.jpg, docs/images/header.jpg -->
> 📷 *Photos of the board (top, with antenna and USB, pin header) go here; see
> [images/README.md](images/README.md).*

- 33 × 33 mm, powered by USB-C (5 V), SMA antenna connector, RTC backup battery header.
- A **WCH CH343** USB-serial bridge on the receiver's UART1. Linux's stock `cdc-acm` driver binds
  it: `/dev/ttyACM0`, USB id `1a86:55d3` (**bench** 2026-11-10). Do not load WCH's vendor driver
  at the same time.
- The host baud is forwarded to the receiver's UART: open the port at the receiver's speed
  (460800 by default). `lgrtk` detects other speeds automatically.
- Pin header: UART2 and UART3 (3.3 V), PPS, RTK status, EVENT, RST.
- Firmware on our board: `LG290P03AANR01A06S`, built 2025/09/18 (**bench**).

### What it sends out of the box (bench, 2026-11-10, firmware A06S, indoors)

```text
sentences: GGA 10.1 Hz GLL 10.1 Hz GSA 6.0 Hz GSV 18.9 Hz RMC 10.1 Hz VTG 10.1 Hz
```

No GST, no `$PQTMEPE`: error estimates must be switched on (`lgrtk config apply-default` does it).
Before it knows the time, the receiver reports the date as 1980-01-06 (GPS epoch); `lgrtk` treats
that as "date unknown".

### The stale-bytes quirk (bench, 2026-11-10)

When the port is opened, the CH343 can deliver bytes from the **previous** session that were
left in its FIFO: on our bench an epoch from an hour earlier, its tail spliced into a fresh
sentence. `tcflush` does not reach them. `lgrtk` discards everything read in the first 150 ms
after every open; any other software reading this board should do the same.

## Messages that matter

| Sentence | Gives | Notes |
|---|---|---|
| `GGA` | position, **fix quality** (1 single, 2 DGNSS, 4 RTK fixed, 5 RTK float), satellites, HDOP, altitude + geoid separation, **correction age**, station id | the only place RTK status appears in NMEA |
| `RMC` | validity, speed, course, **date** | |
| `GST` | per-axis 1-sigma (lat, lon, alt) and the error ellipse | off by default; older firmware printed one decimal only (*reported*) |
| `$PQTMEPE` | estimated error north/east/down/2-D/3-D | version 2; enable with `PQTMCFGMSGRATE,W,PQTMEPE,1,2` |
| `$PQTMVERNO` | firmware string | answer to a query |
| `$PQTMSVINSTATUS` | survey-in progress (base mode) | |
| RTCM 3 MSM4/MSM7 | raw observations for post-processing | `RTCM3-107X` … rates |

`$PQTMPVT`'s fix type is only 0–3 (no 2-D/3-D distinction for RTK): use GGA.

## Commands (`$PQTM…`)

Format `$PQTM<NAME>,W|R,<params>*<checksum>\r\n`; replies `$PQTM<NAME>,OK[,values]` or
`$PQTM<NAME>,ERROR,<code>` (1 invalid parameters, 2 failed, 3 unsupported). `$PQTMSRR` and the
restart commands do not answer. Settings live in RAM until `$PQTMSAVEPAR`; some (fix rate,
constellations, rover/base mode) take effect only after a reset (`$PQTMSRR`). `lgrtk::lg290p`
builds every command below, and `PlanConfig()` decides when saving and resetting are needed.

| Command | Purpose |
|---|---|
| `PQTMCFGFIXRATE` | fix interval in ms (100 = 10 Hz, 50 = 20 Hz) |
| `PQTMCFGMSGRATE` | output rate per message ("every N fixes", 0 = off) |
| `PQTMCFGNMEADP` | NMEA decimal places (`3,8,3,2,3,2` gives 0.02 mm resolution) |
| `PQTMCFGRTK` | RTK on/off/RTD, absolute/relative, max correction age 1–600 s |
| `PQTMCFGCNST` | constellations |
| `PQTMCFGELETHD` / `PQTMCFGCNRTHD` | elevation mask (default 5°) / C/N0 mask (default 10 dBHz) |
| `PQTMCFGRCVRMODE` | rover (1) or base (2) |
| `PQTMCFGSVIN` | base: survey-in, or a fixed ECEF position |
| `PQTMCFGPPS` | PPS pulse |
| `PQTMCFGUART` | UART speed (reopen the host port afterwards) |
| `PQTMSAVEPAR` / `PQTMRESTOREPAR` / `PQTMSRR` / `PQTMHOT` / `PQTMWARM` / `PQTMCOLD` | save / factory defaults / reboot / restarts |

## RTK corrections

The receiver needs RTCM 3 corrections from a base within roughly 10–20 km: a network (NTRIP
caster, e.g. swisstopo swipos in Switzerland) or your own base (a second LG290P in base mode).
By default every UART accepts RTCM input, so corrections go into the **same USB port** the
fixes come out of. `lgrtk::NtripClient` does that, sends your position as GGA when the
mountpoint is a virtual reference station, and reconnects by itself.

**Frames.** The corrected position is in the frame of the corrections (CHTRS95/ETRS89 for
swipos, whatever coordinates you typed into your own base); an uncorrected one is in the
broadcast frame (≈ ITRF today). In Switzerland the two differ by about 0.8 m. Declare which CRS
your corrections use and project from there.

## Antenna

The receiver needs an active multi-band antenna. We use the Waveshare **Q39** quadrifilar helix
(28 dB LNA, 3–16 V, L1/L2/L5 + B3I, no Galileo E6 listed): no ground plane needed, light, IP67.
Mount it high with a clear view of the sky; a QFH also picks up reflections from below, so a
small ground disc helps near water or walls.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `Permission denied` on `/dev/ttyACM0` | not in the serial group | `sudo usermod -aG uucp $USER` (Arch) or `dialout` (Debian/Ubuntu), log in again |
| `busy: another process has the port open` | gpsd, ModemManager, a second `lgrtk` | stop it; for ModemManager add `ENV{ID_MM_DEVICE_IGNORE}="1"` in a udev rule |
| garbage, `no reply to PQTMVERNO at any baud` | wrong speed beyond the detected list, or not an LG290P | `lgrtk --baud N probe` |
| quality 0, 0 satellites | indoors, or antenna not connected | put the antenna outside; cold start takes ~30 s |
| no `GST` | off by default | `lgrtk config set msg-rate GST 1` or `lgrtk config apply-default` |
| RTK float never becomes fixed | corrections too far / old, multipath, few satellites | check `age` in `lgrtk watch`, raise the elevation mask to 10–15° |
| `fault: receiver refused …` | unsupported setting on this firmware | the error names the command and code |

## Sources

- Quectel LG290P(03) & LGx80P(03) GNSS Protocol Specification V1.1 (via SparkFun:
  `docs.sparkfun.com/SparkFun_RTK_Postcard/assets/component_documentation/quectel_lg290p03lgx80p03_gnss_protocol_specification_v1-1.pdf`)
- Quectel LG290P(03) GNSS Specification V1.0 (product brief)
- Waveshare wiki "LG290P GNSS RTK Module" and its Raspberry Pi demo (`web_rtk.py`)
- SparkFun LG290P Quadband GNSS RTK Breakout hookup guide and Arduino library v3.1.1
- WCH `ch343ser_linux` README
