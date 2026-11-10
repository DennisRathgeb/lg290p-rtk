# Changelog

## Unreleased

## v0.1.0 (2026-11-10)

First release: library, CLI, NTRIP, projection.

- `StreamFramer`: NMEA + RTCM3 on one stream, checksum and CRC-24Q, resync after noise.
- `nmea`: GGA, RMC, GST, GSA, GSV, VTG, ZDA, `$PQTMEPE`, `$PQTMVERNO`, `$PQTMSVINSTATUS`, command replies.
- `EpochAssembler` and `Fix`: one record per epoch, closes on the learnt sentence pattern.
- `lg290p`: typed `$PQTM` command builders (checked against the protocol spec v1.1),
  read-back, diff-based configuration planner (save/reset only when needed).
- `Lg290pReceiver`: state machine (identify with baud detection, configure, streaming, stale,
  reconnecting, fault), RTCM injection without splitting frames, command queue, `RawRecorder`.
  Discards the stale bytes the CH343 bridge replays after open (found on the bench).
- `NtripClient`: NTRIP v1/v2, chunked transfer, GGA upload, backoff, stall detection, stops on 401.
- `Projection` (PROJ, refuses ballpark transformations and names missing grids) and `LocalFrame`.
- `lgrtk` CLI: probe, watch, status, config, reset, base, ntrip, project.
- CMake package (`find_package(lgrtk)`), pkg-config file, examples, Doxygen docs.
