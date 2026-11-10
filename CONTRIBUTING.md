# Contributing

Same conventions as the `ar-stack` repository this toolkit came from.

## Workflow

1. An issue first (what and why). Branch `issue-<n>-<slug>` from `main`.
2. Small commits, each building and passing `make test`.
3. A pull request into `main`; CI (build + tests on Ubuntu 22.04/gcc 11, 24.04/gcc, 24.04/clang,
   format, guards, tidy, docs) must be green. Squash merge.
4. A change in behaviour changes the documentation in the same PR (README, `docs/hardware.md`,
   the header comments). Add a line to `CHANGELOG.md` under *Unreleased*.
5. Releases: tag `vX.Y.Z` on `main` after updating `CHANGELOG.md` and `kVersion`
   (`src/version.cc`, `CMakeLists.txt`, `Doxyfile`).

## Code

- C++20, Google C++ Style Guide. Formatting is `.clang-format` (`BasedOnStyle: Google`), naming
  is `.clang-tidy`; neither file is edited to make code pass: fix the code. `make fmt` formats.
- Headers `.h` with include guards `LGRTK_<PATH>_H_` (`tools/check-header-guards.py`), sources `.cc`.
- Everything public in `include/lgrtk/` is documented (Doxygen runs with warnings as errors).
- The hot path (read → frame → parse → assemble → callback) does not allocate.
- Tests first (GoogleTest). New receiver behaviour gets a case in the simulated LG290P
  (`tests/sim/`), never a test that needs hardware. Hardware findings go into
  `docs/hardware.md` marked **bench** with date and firmware, and into a regression test.
- Decisions taken without the maintainer are recorded in `docs/decisions.md`.

## Before a PR

```sh
make hooks     # once: pre-commit runs clang-format and the guard check on staged files
make check     # lint, tests (ASan/UBSan, -Werror), clang-tidy, Doxygen
```
