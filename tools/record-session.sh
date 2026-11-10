#!/usr/bin/env bash
# Records an LG290P session through lgrtkd until Ctrl-C. The receiver is used
# read-only (apply_config = false). Needs a built tree (make build) and python3.
#
#   tools/record-session.sh NAME                      # without corrections
#   tools/record-session.sh NAME HOST MOUNT USER      # with NTRIP
#
# Environment:
#   LGRTK_PORT                 serial device (default: the CH343 under
#                              /dev/serial/by-id, else /dev/ttyACM0)
#   LGRTK_DATA                 output root (default ~/.local/share/lgrtk/recordings)
#   LGRTK_BIN                  directory with lgrtkd and lgrtk (default build/dev)
#   LGRTK_NTRIP_PASSWORD_FILE  NTRIP password, one line (required with HOST)
#
# Writes $LGRTK_DATA/<date>-<NAME>/:
#   raw.bin        every byte the receiver sent (NMEA + RTCM), replayable
#   gpsd.jsonl     every TPV/SKY/GST/DEVICES report with its arrival time
#   status.log     `lgrtk ctl status` every 10 s (state, counters, NTRIP)
#   lgrtkd.log     daemon log (state changes, NTRIP connects)
#   notes.txt      what you type at the end (place, sky, antenna, ...)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${LGRTK_BIN:-$HERE/../build/dev}
USAGE="usage: record-session.sh NAME [HOST MOUNT USER]"
NAME=${1:?$USAGE}
HOST=${2:-}
MOUNT=${3:-}
USER_=${4:-}
if [ -n "$HOST" ] && { [ -z "$MOUNT" ] || [ -z "$USER_" ]; }; then
  echo "$USAGE" >&2
  exit 2
fi
OUT=${LGRTK_DATA:-$HOME/.local/share/lgrtk/recordings}/$(date +%F)-$NAME
mkdir -p "$OUT"
# Stable path of the Waveshare board (CH343) whatever ttyACM number it gets.
PORT=${LGRTK_PORT:-}
if [ -z "$PORT" ]; then
  PORT=/dev/ttyACM0
  for dev in /dev/serial/by-id/usb-1a86_*; do
    [ -e "$dev" ] && PORT=$dev && break
  done
fi
echo "receiver: $PORT -> $(readlink -f "$PORT")"
RUN=$(mktemp -d)
SOCK=$RUN/control.sock

cat >"$OUT/lgrtkd.toml" <<EOF
[receiver]
port = "$PORT"
apply_config = false
[gpsd]
listen = "127.0.0.1:2947"
[raw]
listen = "127.0.0.1:5018"
[control]
sock = "$SOCK"
EOF
if [ -n "$HOST" ]; then
  PW=${LGRTK_NTRIP_PASSWORD_FILE:?set LGRTK_NTRIP_PASSWORD_FILE to the password file}
  [ -r "$PW" ] || { echo "cannot read $PW" >&2; exit 2; }
  cat >>"$OUT/lgrtkd.toml" <<EOF
[ntrip]
enabled = true
host = "$HOST"
mountpoint = "$MOUNT"
user = "$USER_"
password_file = "$PW"
EOF
fi

"$BIN/lgrtkd" --config "$OUT/lgrtkd.toml" 2>"$OUT/lgrtkd.log" &
DAEMON=$!
sleep 2
kill -0 "$DAEMON" 2>/dev/null || { cat "$OUT/lgrtkd.log"; exit 1; }

# Raw tap and gpsd JSON, each to its own file.
python3 -I - "$OUT" <<'PY' &
import json, socket, sys, threading, time
out = sys.argv[1]
def raw():
    s = socket.create_connection(("127.0.0.1", 5018))
    with open(out + "/raw.bin", "ab") as f:
        while (b := s.recv(65536)):
            f.write(b); f.flush()
def gpsd():
    s = socket.create_connection(("127.0.0.1", 2947))
    s.sendall(b'?WATCH={"enable":true,"json":true};\n')
    with open(out + "/gpsd.jsonl", "a") as f:
        for line in s.makefile("r"):
            f.write(json.dumps({"t": time.time(), "msg": json.loads(line)}) + "\n")
            f.flush()
threading.Thread(target=raw, daemon=True).start()
gpsd()
PY
REC=$!

stop() {
  kill "$REC" 2>/dev/null || true
  kill -TERM "$DAEMON" 2>/dev/null || true
  wait "$DAEMON" 2>/dev/null || true
  rm -rf "$RUN"
  echo
  echo "recorded to $OUT:"
  ls -la "$OUT"
  read -r -p "notes (place, sky view, antenna, anything odd): " n || true
  echo "$n" >"$OUT/notes.txt"
  exit 0
}
trap stop INT TERM

echo "recording to $OUT - Ctrl-C to stop"
while true; do
  {
    echo "=== $(date +%T)"
    "$BIN/lgrtk" ctl --socket "$SOCK" status 2>&1 || true
  } >>"$OUT/status.log"
  # one live line: fix type, sats, sigma, correction age
  tail -n 200 "$OUT/gpsd.jsonl" 2>/dev/null | python3 -I "$HERE/live_line.py" || true
  sleep 10
done
