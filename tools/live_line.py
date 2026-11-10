#!/usr/bin/env python3
"""Prints one status line from the newest TPV in a gpsd.jsonl tail (stdin).

Used by tools/record-session.sh.
"""

import json
import sys

tpv = None
for line in sys.stdin:
    try:
        m = json.loads(line)["msg"]
    except (ValueError, KeyError):
        continue
    if m.get("class") == "TPV":
        tpv = m
if tpv:
    mode = tpv.get("mode", 0)
    names = {1: "single", 2: "dgnss", 3: "RTK-FIXED", 4: "rtk-float"}
    st = "no-device" if mode == 0 else "no-fix" if mode == 1 else names.get(tpv.get("status"), "?")
    print("{}  {:9}  epx {} m  epv {} m  corr-age {} s".format(
        tpv.get("time", "-"), st, tpv.get("epx", "-"), tpv.get("epv", "-"), tpv.get("dgpsAge", "-")))
