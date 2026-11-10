#!/usr/bin/env python3
"""Check the include guards of every tracked header (Google style).

A header is a `.h` file whose first code lines are

    #ifndef LGRTK_<PATH>_<FILE>_H_
    #define LGRTK_<PATH>_<FILE>_H_

and whose last line is `#endif  // LGRTK_<PATH>_<FILE>_H_`. <PATH> is the
directory below include/lgrtk/ for public headers, else the path from the
repository root (tests/sim/x.h -> LGRTK_TESTS_SIM_X_H_). `#pragma once` and
`.hpp` are errors.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def expected_guard(rel: str) -> str:
    stem = rel.rsplit(".", 1)[0]
    if stem.startswith("include/lgrtk/"):
        stem = stem[len("include/lgrtk/") :]
    return "LGRTK_" + re.sub(r"[^A-Za-z0-9]", "_", stem).upper() + "_H_"


def check(rel: str) -> str | None:
    if rel.endswith(".hpp"):
        return "header must use the .h extension"
    text = (ROOT / rel).read_text(encoding="utf-8")
    if "#pragma once" in text:
        return "use an include guard, not #pragma once"
    guard = expected_guard(rel)
    code = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith("//")]
    if len(code) < 3:
        return "too short to carry a guard"
    if code[0] != f"#ifndef {guard}" or code[1] != f"#define {guard}":
        return f"guard must be {guard}"
    if text.rstrip().splitlines()[-1] != f"#endif  // {guard}":
        return f"last line must be '#endif  // {guard}'"
    return None


def main() -> int:
    files = subprocess.run(
        ["git", "ls-files", "*.h", "*.hpp"], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.split()
    bad = [(f, err) for f in files if (err := check(f))]
    for f, err in bad:
        print(f"{f}: {err}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
