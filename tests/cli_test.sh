#!/usr/bin/env bash
# End-to-end test of the lgrtk CLI against the simulated receiver.
# Usage: cli_test.sh <lgrtk> <sim_tool>
set -euo pipefail
LGRTK=$1
SIM=$2
fail() { echo "FAIL: $*" >&2; exit 1; }

coproc SIMPROC { "$SIM"; }
read -r PORT <&"${SIMPROC[0]}"
SIM_STDIN=${SIMPROC[1]}
SIM_PID=$SIMPROC_PID
# Closing the simulator's stdin makes it exit cleanly (and remove its temp dir).
cleanup() { eval "exec ${SIM_STDIN}>&-"; wait "$SIM_PID" 2>/dev/null || true; }
trap cleanup EXIT

out=$("$LGRTK" --port "$PORT" probe --seconds 2) || fail "probe exit $?"
grep -q "firmware: LG290P03AANR01A06S" <<<"$out" || fail "probe: no firmware line: $out"
grep -q "GGA" <<<"$out" || fail "probe: no GGA rate: $out"

out=$("$LGRTK" --port "$PORT" config get fix-rate) || fail "config get exit $?"
grep -q "100" <<<"$out" || fail "config get fix-rate: $out"

"$LGRTK" --port "$PORT" config set msg-rate GST 1 >/dev/null || fail "config set exit $?"
out=$("$LGRTK" --port "$PORT" config get msg-rate GST) || fail "config get msg exit $?"
grep -q "1" <<<"$out" || fail "msg-rate GST not 1: $out"

set +e
"$LGRTK" --port "$PORT" reset factory >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "reset factory without --yes should exit 2, got $rc"

set +e
"$LGRTK" frobnicate >/dev/null 2>&1
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "unknown command should exit 2, got $rc"

out=$("$LGRTK" --port "$PORT" watch --count 3) || fail "watch exit $?"
[ "$(grep -c "single" <<<"$out")" -ge 3 ] || fail "watch: $out"

"$LGRTK" version | grep -q "lgrtk 0.1.0" || fail "version"
echo "cli_test: ok"
