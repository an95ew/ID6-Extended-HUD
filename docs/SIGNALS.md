# Reference Signal Definitions

These signal definitions are reference candidates for the Gen 1 B58 platform (tested on a 2018 BMW F34 340i, B58, 8-speed automatic) in the current source tree. They are not a guaranteed compatibility matrix for other engine families.

Requests use HSFZ TCP port `6801` and UDS service `22` unless marked OBD. Positive UDS replies begin with `62` followed by the requested DID; the table below describes the payload bytes following that prefix.

| Signal | ECU | DID or PID | Reference Conversion |
| --- | --- | --- | --- |
| Boost | DME `12` | `4205` and `4201` | Two big-endian bytes each. Absolute pressure (kPa) = raw ÷ 128; ambient pressure (kPa) = raw ÷ 256. Collector subtracts ambient and converts to psi. |
| AFR | DME `12` | `5889` | Two bytes. Lambda = raw × 16 ÷ 65535, converted to gasoline-equivalent AFR. |
| Ignition timing | DME `12` | `4A49` | Two signed big-endian bytes ÷ 10. Raw `FFFF` is treated as invalid/blank. |
| Intake air temperature | DME `12` | `580F` | One byte: raw × 0.75 − 48 °C. Raw `FF` is rejected. |
| Transmission temperature | EGS `18` | `DA12` | Two big-endian bytes: raw − 48 °C. |
| Rail pressure | DME `12` | `56D7` | Two big-endian bytes: raw ÷ 50 bar. |
| Oil pressure | DME `12` | `586F` | Two big-endian bytes: raw ÷ 1000 bar. |
| Coolant temperature | DME `12` | `4300` | Two big-endian bytes: raw ÷ 10 − 273.15 °C. |
| ShiftDeck RPM | DME `12` | OBD `01 0C` | Two big-endian bytes ÷ 4. |
| ShiftDeck speed | DME `12` | OBD `01 0D` | One byte in km/h, displayed in mph. |

The worker enforces payload length, plausibility bounds, and staleness checks; refer to the source files for exact limits and units. If an ECU exposes coolant temperature via OBD `01 05`, validate it against the intended coolant channel before adding a fallback. OBD PIDs and BMW UDS DIDs use distinct request and reply formats.

Source references: [collector.c](../native/src/collector.c) · [gauge_core.c](../native/src/gauge_core.c)
