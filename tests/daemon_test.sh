#!/usr/bin/env bash
# End-to-end test of lgrtkd against the simulated receiver: gpsd clients, raw
# tap, control socket via `lgrtk ctl`, unplug/replug, SIGTERM, bad config.
# Usage: daemon_test.sh <lgrtkd> <sim_tool> <lgrtk>
set -euo pipefail
LGRTKD=$1
SIM=$2
LGRTK=$3
fail() {
  echo "FAIL: $*" >&2
  [ -f "${DIR:-}/lgrtkd.log" ] && sed 's/^/  lgrtkd: /' "$DIR/lgrtkd.log" >&2
  exit 1
}

DIR=$(mktemp -d)
free_port() { python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }
GPSD_PORT=$(free_port)
RAW_PORT=$(free_port)
NTRIP_PORT=$(free_port)  # nothing listens: the client keeps retrying

coproc SIMPROC { "$SIM"; }
read -r TTY <&"${SIMPROC[0]}"
SIM_STDIN=${SIMPROC[1]}
SIM_PID=$SIMPROC_PID
DAEMON_PID=
cleanup() {
  [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
  eval "exec ${SIM_STDIN}>&-"
  wait "$SIM_PID" 2>/dev/null || true
  rm -rf "$DIR"
}
trap cleanup EXIT

cat >"$DIR/lgrtkd.toml" <<EOF
[receiver]
port = "$TTY"
[gpsd]
listen = "127.0.0.1:$GPSD_PORT"
device_name = "/dev/sim"
[raw]
listen = "127.0.0.1:$RAW_PORT"
[control]
sock = "$DIR/control.sock"
[ntrip]
enabled = true
host = "127.0.0.1"
port = $NTRIP_PORT
mountpoint = "TEST"
EOF

"$LGRTKD" --config "$DIR/lgrtkd.toml" --check-config >/dev/null || fail "check-config"
printf '[receiver]\nprot = "x"\n' >"$DIR/bad.toml"
set +e
"$LGRTKD" --config "$DIR/bad.toml" 2>"$DIR/bad.err"
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "bad config should exit 2, got $rc"
grep -q "receiver.prot" "$DIR/bad.err" || fail "bad config error does not name the key"

"$LGRTKD" --config "$DIR/lgrtkd.toml" 2>"$DIR/lgrtkd.log" &
DAEMON_PID=$!

# A gpsd client in Python: VERSION, WATCH, then 3 TPV with mode 3.
gpsd_client() {
  python3 -I - "$GPSD_PORT" "$1" <<'PY'
import json, socket, sys, time
port, want = int(sys.argv[1]), sys.argv[2]
deadline = time.time() + 15
while True:
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        break
    except OSError:
        if time.time() > deadline:
            sys.exit("cannot connect")
        time.sleep(0.1)
f = s.makefile("r")
hello = json.loads(f.readline())
assert hello["class"] == "VERSION", hello
s.sendall(b'?WATCH={"enable":true,"json":true};\n')
s.settimeout(15)
tpv = 0
seen = set()
for line in f:
    obj = json.loads(line)
    seen.add(obj["class"])
    if want == "tpv" and obj["class"] == "TPV" and obj.get("mode") == 3:
        tpv += 1
        if tpv == 3:
            print("ok", sorted(seen))
            break
PY
}
out=$(gpsd_client tpv) || fail "gpsd client: $out"
grep -q "^ok" <<<"$out" || fail "gpsd client: $out"

# Raw tap: receiver bytes including GGA.
python3 -I - "$RAW_PORT" <<'PY' || fail "raw tap"
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
buf = b""
end = time.time() + 5
while b"GGA" not in buf and time.time() < end:
    buf += s.recv(4096)
assert b"GGA" in buf, buf[:200]
PY

out=$("$LGRTK" ctl --socket "$DIR/control.sock" status) || fail "ctl status: $out"
grep -q "state: streaming" <<<"$out" || fail "ctl status: $out"
grep -q "gpsd_clients:" <<<"$out" || fail "ctl status: $out"
grep -q "ntrip_state:" <<<"$out" || fail "ctl status without NTRIP: $out"

# apply-default restarts the receiver and the NTRIP client while fixes flow.
for _ in 1 2 3 4 5; do
  "$LGRTK" ctl --socket "$DIR/control.sock" apply-default >/dev/null || fail "ctl apply-default"
  sleep 0.1
done
"$LGRTK" ctl --socket "$DIR/control.sock" reset hot >/dev/null || fail "ctl reset hot"
out=$(gpsd_client tpv) || fail "no fixes after apply-default: $out"

# Unplug: a watching client sees the device deactivated and a mode-0 TPV,
# then fixes again after replug.
python3 -I - "$GPSD_PORT" <<'PY' &
import json, socket, sys
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=20)
f = s.makefile("r")
f.readline()
s.sendall(b'?WATCH={"enable":true,"json":true};\n')
state = "wait-fix"
for line in f:
    o = json.loads(line)
    if state == "wait-fix" and o["class"] == "TPV" and o.get("mode") == 3:
        state = "wait-loss"
    elif state == "wait-loss" and o["class"] == "DEVICES" and o["devices"][0]["activated"] == 0:
        state = "wait-nofix"
    elif state == "wait-nofix" and o["class"] == "TPV" and o["mode"] == 0:
        state = "wait-back"
    elif state == "wait-back" and o["class"] == "TPV" and o.get("mode") == 3:
        print("recovered")
        break
PY
WATCHER=$!
sleep 1
echo unplug >&"$SIM_STDIN"
sleep 2
echo replug >&"$SIM_STDIN"
for _ in $(seq 1 100); do
  kill -0 "$WATCHER" 2>/dev/null || break
  sleep 0.1
done
if kill -0 "$WATCHER" 2>/dev/null; then
  kill "$WATCHER"
  fail "watcher did not see loss and recovery within 10 s"
fi
wait "$WATCHER" || fail "watcher failed"

# SIGTERM: exit 0 within 2 s.
kill -TERM "$DAEMON_PID"
for _ in $(seq 1 20); do
  kill -0 "$DAEMON_PID" 2>/dev/null || break
  sleep 0.1
done
kill -0 "$DAEMON_PID" 2>/dev/null && fail "lgrtkd still running 2 s after SIGTERM"
wait "$DAEMON_PID" || fail "lgrtkd exit status $? after SIGTERM"
DAEMON_PID=
[ -e "$DIR/control.sock" ] && fail "control socket left behind"
echo "daemon_test: ok"
