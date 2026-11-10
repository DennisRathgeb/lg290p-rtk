# lgrtk API {#mainpage}

C++20 library for the Quectel LG290P. Start with these classes:

| Class | What it does |
|---|---|
| lgrtk::Lg290pReceiver | The driver: state machine on its own thread, fixes by callback, RTCM injection |
| lgrtk::Fix | One epoch: position, fix type, 1-sigma, velocity, correction age, timestamps |
| lgrtk::NtripClient | NTRIP v1/v2 corrections into the receiver |
| lgrtk::Projection | PROJ transformation built once (e.g. LV95 + LHN95) |
| lgrtk::LocalFrame | Tangent-plane ENU around an origin, no PROJ |
| lgrtk::lg290p | Typed `$PQTM` commands and the diff-based configuration planner |
| lgrtk::StreamFramer, lgrtk::nmea | The parsing layers underneath |

See the repository README for the module, the state machine and examples.
