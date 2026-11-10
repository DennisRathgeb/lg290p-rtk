# Working in this repository

- Read `README.md`, `docs/architecture.md` and `docs/decisions.md` first; conventions and workflow
  are in `CONTRIBUTING.md`. `make check` must be clean before a pull request.
- CI never needs hardware: extend the simulated LG290P (`tests/sim/`) or the fake caster.
  Behaviour found on a real receiver goes into `docs/hardware.md` (marked **bench**, with date and
  firmware) and into a test.
- The protocol authority is the Quectel LG290P protocol specification v1.1; cite it when a
  command or field layout changes.
- NTRIP credentials never go into code, tests, logs or commits.
