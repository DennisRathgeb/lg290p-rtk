# lgrtkd: the LG290P as a system-wide GNSS source

`lgrtkd` owns one receiver (and optionally an NTRIP caster connection) and serves it to everything
else on the machine:

| Output | Default | For |
|---|---|---|
| **gpsd JSON protocol** on TCP | `127.0.0.1:2947` | `cgps`, `gpspipe -w`, QGIS, OpenCPN, Python `gps`, anything that reads gpsd JSON (not `gpsmon`, which needs gpsd's raw mode) |
| **Raw tap** on TCP | off | the untouched receiver bytes (NMEA + RTCM): loggers, RTKLIB `str2str`, post-processing |
| **chrony** SOCK refclock | off | system time from the receiver's UTC |
| **Control socket** | `/run/lgrtkd/control.sock` | `lgrtk ctl status / reset / apply-default` |

It is built on the same library as the `lgrtk` tool, so it gets the same state machine: baud
detection, configuration by read-back, recovery from unplugging and receiver resets, stale-data
detection. One process owns the serial port (decision D6): while `lgrtkd` runs, use `lgrtk ctl`
instead of `lgrtk --port …`.

```text
              ┌──────────────── lgrtkd ────────────────┐
/dev/lg290p0 ─┤ Lg290pReceiver ──callbacks──► event loop ├─► gpsd clients (TCP 2947)
              │        ▲                    (1 thread)  ├─► raw tap clients
NTRIP caster ─┤ NtripClient ┘ corrections               ├─► chrony (unix datagram)
              │                                         ├─◄ lgrtk ctl (unix socket)
              └─────────────────────────────────────────┘
```

The receiver and NTRIP threads only hand work to one event-loop thread, which serves every socket
without blocking. A client that stops reading is disconnected when its 256 KiB buffer fills; it
never slows the receiver or the other clients.

**Bench (2026-11-10, Waveshare board, firmware A06S, indoors, `apply_config = false`):** lgrtkd
identified the receiver at 460800 baud, served TPV and SKY at 10 Hz to a gpsd client (mode 1
indoors, no fix), `lgrtk ctl status` reported `streaming` with 0 bad sentences, the raw tap carried
the receiver bytes, and SIGTERM stopped it in about 0.2 s. The systemd unit, chrony and RTK
through the daemon are not yet tested on hardware.

## Install

**Arch Linux** (from the repository):

```sh
cd packaging && makepkg -si
```

**Any distribution** with CMake:

```sh
cmake --preset release -DCMAKE_INSTALL_PREFIX=/usr -DLGRTK_SYSCONFDIR=/etc
cmake --build --preset release
sudo cmake --install build/release
sudo install -Dm644 /usr/share/lgrtk/lgrtkd.toml /etc/lgrtk/lgrtkd.toml
```

Building the daemon needs `nlohmann/json` and `toml++` (Arch: `nlohmann-json tomlplusplus`,
Ubuntu: `nlohmann-json3-dev`); CMake downloads them when they are missing. The library itself
needs neither.

Then:

```sh
sudo systemd-sysusers                 # creates the "lgrtk" user (member of uucp/dialout)
sudo udevadm control --reload && sudo udevadm trigger   # /dev/lg290p0 appears
sudo systemctl enable --now lgrtkd
journalctl -u lgrtkd -f               # receiver and NTRIP state changes
sudo usermod -aG lgrtk "$USER"        # to use `lgrtk ctl` without sudo (log in again)
```

| File | Purpose |
|---|---|
| `/usr/bin/lgrtkd` | the daemon |
| `/etc/lgrtk/lgrtkd.toml` | configuration (below) |
| `/etc/credstore/ntrip-password` | NTRIP password (optional), one line, mode 0600, owned by root; systemd passes it in (`ImportCredential=`) |
| `/usr/lib/systemd/system/lgrtkd.service` | `Type=notify`, watchdog, hardened, runs as `lgrtk`; loads `cdc_acm`/`usbserial` first so a receiver plugged in later is allowed |
| `/usr/lib/udev/rules.d/99-lg290p.rules` | `/dev/lg290p0` for the CH343 (`1a86:55d3`), ModemManager off |
| `/usr/lib/sysusers.d/lgrtk.conf` | the `lgrtk` system user |

## Configuration

`/etc/lgrtk/lgrtkd.toml`; every key is optional. An unknown key or a wrong type stops the daemon
with the key named (exit 2), so a typo is never silently ignored. Check a file with
`lgrtkd --check-config --config FILE`.

| Key | Default | Meaning |
|---|---|---|
| `receiver.port` | `/dev/ttyACM0` | serial device; use the udev name `/dev/lg290p0` |
| `receiver.baud` | `460800` | tried first; other speeds are detected |
| `receiver.apply_config` | `true` | read back the receiver settings and write only differences (default rover configuration: 10 Hz, GGA/RMC/GST/EPE, RTK on) |
| `gpsd.listen` | `127.0.0.1:2947` | `host:port` or `[v6]:port`; `0.0.0.0` to serve the network |
| `gpsd.device_name` | `receiver.port` | the `device` clients see |
| `raw.listen` | off | `host:port` of the raw tap |
| `chrony.sock` | off | chrony's SOCK refclock path |
| `control.sock` | `/run/lgrtkd/control.sock` | control socket; `""` = off |
| `ntrip.enabled` | `false` | connect to a caster |
| `ntrip.host`, `ntrip.port`, `ntrip.mountpoint`, `ntrip.user` | –, `2101`, –, – | caster |
| `ntrip.password_file` | – | password file; empty = systemd credential `ntrip-password` |
| `ntrip.version` | `2` | NTRIP 1 or 2 |
| `ntrip.send_gga` | `true` | send the rover position (VRS and nearest-base mountpoints need it) |

Example for swipos (Switzerland):

```toml
[receiver]
port = "/dev/lg290p0"

[ntrip]
enabled = true
host = "www.swipos.ch"
port = 2101
mountpoint = "MSM_GISGEO_LV95LHN95"  # example; list them: lgrtk ntrip sourcetable www.swipos.ch
user = "your-user"
```

and the password in `/etc/credstore/ntrip-password` (`sudo install -m600 /dev/stdin
/etc/credstore/ntrip-password`, type it, Ctrl-D). Never put it in the TOML file: the daemon never
logs it, and the unit hands it over through `ImportCredential=`.

## gpsd protocol

Supported requests: `?VERSION;`, `?WATCH={…};` (`enable`, `json`, `nmea`; a WATCH object enables
watching unless it says `"enable":false`), `?POLL;`, `?DEVICES;`; several on one line work
(`?VERSION;?DEVICES;`). At most 64 clients per socket.
Reports: `VERSION` on connect, `DEVICES`, `WATCH`, `TPV`, `GST`, `SKY`, `POLL`, `ERROR`. Raw and
binary watch modes, `?DEVICE=` (changing speed or mode) and PPS reports are not supported: the
receiver is configured through `lgrtk ctl` instead.

How a fix becomes a TPV:

| TPV field | From |
|---|---|
| `mode` | 3 with a position (the LG290P always reports height), 1 without, 0 when the receiver is lost |
| `status` | 3 RTK fixed, 4 RTK float, 2 DGNSS (also SBAS), 1 autonomous, from GGA quality |
| `time` | the epoch's UTC (only once the receiver knows the date; never 1980/1970) |
| `lat`, `lon`, `altHAE`, `altMSL`, `geoidSep` | GGA; `altHAE` = altitude + geoid separation |
| `epx`, `epy`, `epv` | 1.96 × the 1-sigma errors (about 95 %) from GST or `$PQTMEPE` |
| `speed`, `track` | RMC/VTG |
| `dgpsAge`, `dgpsSta` | GGA correction age and station |

`GST` carries the 1-sigma errors themselves (`lat`, `lon`, `alt`). `SKY` lists every satellite
from GSV (signals of one satellite merged, strongest C/N0 kept) with `used` from GSA.

When the receiver is unplugged, watching clients get `DEVICES` with `activated: 0` and a TPV with
`mode: 0`; they stay connected and get fixes again as soon as the receiver is back.

Quick checks: `cgps`, `gpspipe -w`, or without gpsd tools:

```sh
printf '?WATCH={"enable":true,"json":true};\n' | nc 127.0.0.1 2947
```

**Port 2947 is gpsd's.** Stop gpsd (`systemctl disable --now gpsd.socket gpsd`) or give lgrtkd
another port and point clients at it (`cgps localhost:2948`).

## Raw tap

```toml
[raw]
listen = "127.0.0.1:5018"
```

Every byte the receiver sends, unmodified. Record with `nc 127.0.0.1 5018 > session.raw`, or feed
RTKLIB: `str2str -in tcpcli://127.0.0.1:5018 -out file://rover_%Y%m%d%h.rtcm3`. To get raw
observations for post-processing, enable the receiver's RTCM MSM output first. `lgrtk ctl`
cannot do that yet: stop lgrtkd, run `lgrtk config set msg-rate RTCM3-107X 1` (saved to the
receiver's flash) and start lgrtkd again.

## chrony

```toml
[chrony]
sock = "/run/chrony.lgrtk.sock"
```

and in `chrony.conf`:

```text
refclock SOCK /run/chrony.lgrtk.sock refid GNSS offset 0.0 delay 0.2 noselect
```

The sample is the epoch's UTC against the system time at which its last sentence was read, so it
includes the receiver's output latency (tens of ms at 460800 baud). Samples are sent whatever the
offset, so a board without RTC that boots far off can be set by chrony (`makestep`). Calibrate
`offset` against a good NTP source (`chronyc sourcestats`) before dropping `noselect`; for
sub-millisecond time use the PPS pin (not yet supported). chronyd creates the socket; lgrtkd must
be allowed to write to it (chrony runs as root or `chrony`: check its permissions). Untested on
hardware so far.

## Control

```sh
lgrtk ctl status                 # state, firmware, baud, counters, clients, NTRIP
lgrtk ctl reset hot|warm|cold|soft
lgrtk ctl apply-default          # restart the receiver with the default rover configuration
```

`--socket PATH` selects another socket. A factory reset is refused over the socket: stop the
daemon and use `lgrtk reset factory --yes`.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `listen 127.0.0.1:2947: Address already in use` | gpsd is running; stop it or change `gpsd.listen` |
| state stays `identifying` | wrong port, or another program has it open (`fuser /dev/lg290p0`) |
| `lgrtk ctl`: permission denied | add yourself to the `lgrtk` group |
| `lgrtk --port /dev/lg290p0 …`: device busy | lgrtkd owns the port; use `lgrtk ctl` |
| NTRIP `auth-failed` in the journal | wrong user/password; the client stops retrying until restarted |
| no `/dev/lg290p0` | udev rule not loaded (`udevadm control --reload`), or a different USB bridge |
