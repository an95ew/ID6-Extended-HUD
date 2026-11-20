---
name: dme-reads
description: Read live engine telemetry (RPM, speed) from the DME via ExtendedHUD's read-only OBD collector (native/src/collector.c and native/src/diag_link.c, built as shift-deck-qnx): OBD 01 0C / 01 0D requests, HSFZ vs. MOST UDS tunnel fallback, diagnostic lease sharing, raw tracing, and UDP drive_frame packets. Use when troubleshooting missing engine telemetry or before adding any new ECU read.
---

# Read-Only Engine Telemetry (DME)

`native/src/collector.c` builds ExtendedHUD's strictly read-only OBD collector (`shift-deck-qnx`). It issues only OBD service `01` requests and never writes to any ECU.

| Binary (`build_hud.sh`) | Requests | Target ECU | Output |
|---|---|---|---|
| `shift-deck-qnx` | OBD **`01 0C`** (RPM), **`01 0D`** (speed) | DME `0x12` | UDP `struct drive_frame {u64 sampled_ms; i32 rpm; i32 kph; ...}` → `127.0.0.1:<port>` |

No binary in this repository sends UDS write/control services (`0x14`, `0x2C`, `0x2E`, `0x2F`, `0x31`, `0x3D`). `--session` controls the local diagnostic lease lock (`diagnostic_lease.h`), not UDS `0x10`.

## Transports (`native/src/diag_link.c`)

```text
collector ──HSFZ TCP 6801──▶ ZGW (gateway-ip.txt, default 169.254.21.3) ──▶ DME 0x12
      └─ fallback ──DSI──▶ DSYSMOSTServer ──MOST tunnel (fkt 0x050, op C)──▶ DME 0x12
```

- `--transport auto` (default): uses HSFZ if the configured gateway responds, otherwise falls back to the MOST UDS tunnel after 5 failed HSFZ attempts. `--transport hsfz` requires `--host`; `--transport most` ignores `--host`.
- **Reference test platform (2018 BMW F34 340i, B58, NBT Evo `NBTevo_X20511I`, split-node):** No HSFZ diagnostic gateway responds on ENET (HSFZ ident broadcast to UDP `6811` gets no reply; the only ENET neighbor `169.254.144.39` does not expose `6801`/`6811`/`22`/`80`), so `shift-deck-qnx` automatically falls back to **MOST** from `hu-omap`.
- **MOST tunnel timing (verified on reference hardware):** ~270 ms per request (~550 ms per RPM + speed pair). Consumers must set their staleness limit comfortably above this; ExtendedHUD uses 2 s (`HUD_DRIVE_STALE_MS`; `drive_frame.h`'s 750 ms default targets HSFZ).
- The collector monitors its parent process (`getppid()`) and exits cleanly if orphaned; `shift-deck-qnx` refuses to start standalone without a parent.

## Raw Exchange Tracing

```bash
# SHIFTDECK_TRACE=1 prints raw MOST exchanges to stderr (opt-in; silent otherwise):
ssh root@169.254.199.99 'on -f hu-omap sh -c "export LD_LIBRARY_PATH=/lib:/usr/lib:/boot/lib:/opt/sys/lib SHIFTDECK_TRACE=1; \
  /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/shift-deck-qnx --host 169.254.21.3 --udp-port 42899 --cycles 60 2>&1"'
# Example reply: p=f06371010f0b120004410c0000 -> 41 0C 00 00 = 0 rpm (RPM = (A*256+B)/4); 41 0D 00 = 0 km/h
```

The MOST tunnel reply payload layout is:
`f0 63 71 01 0f 0b <ecu_addr> 00 <len> <uds_reply...>`

## Operational Pitfalls

- **Never use `fork()` in threaded QNX processes:** Once `hud-sportbar-qnx` initializes MOST/DSI (which spawns background threads), QNX refuses `fork()`. Always spawn child processes with `posix_spawn()` (C) or `spawn()` (C++, `diagnostic_worker.h`).
- **UDP port isolation on `hu-omap`:** UDP ports are shared across all processes on the HMI node. Coexisting plugins such as ShiftDeck bind `DRIVE_PORT` `42863` (and BoostDeck binds `42860`). ExtendedHUD uses dedicated port **`42873`**. Inspect bindings with `on -f hu-omap sockstat`.
- **`--cycles N` includes startup turns:** Loop iterations before the MOST link comes up count toward `--cycles`; use `>= 60` for diagnostic tests so the collector does not exit before connecting.
- **Ignition on, engine off:** The DME responds normally with `0` RPM. Only ask the operator to start the engine when non-zero RPM verification is explicitly required (parked, P, handbrake engaged, outdoors).
- **New ECU signals:** Any additional diagnostic PID/DID read must remain strictly read-only (OBD `01` / UDS `22`), log raw request/response frames during testing, and be validated against an independent reference reading.
