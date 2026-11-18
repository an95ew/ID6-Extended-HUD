---
name: dme-reads
description: Read live engine values (RPM, speed) from the DME on this car through ExtendedHUD's read-only OBD collector (native/src/collector.c and native/src/diag_link.c, built as shift-deck-qnx): OBD 01 0C / 01 0D requests, HSFZ vs the MOST tunnel, the diagnostic gateway, tracing, and the UDP drive_frame packets ExtendedHUD consumes. Use when engine data is missing (the HUD stays blank while revving) or before any new request goes to an ECU.
---

# Engine data from the DME

`native/src/collector.c` builds ExtendedHUD's strictly read-only OBD collector (`shift-deck-qnx`). It sends only OBD service `01` and never writes to any ECU.

| Binary (`build_hud.sh`) | Sends | To | Out |
|---|---|---|---|
| `shift-deck-qnx` | OBD **01 0C** (RPM), **01 0D** (speed) only | DME 0x12 | UDP `struct drive_frame {u64 sampled_ms; i32 rpm; i32 kph}` → 127.0.0.1:port |

No build in this repository sends `0x14`, `0x2C`, `0x2E`, `0x2F`, `0x31`, or `0x3D`. `--session` is a local gateway-lease lock (`diagnostic_lease.h`), not UDS `0x10`.

## Transports (`native/src/diag_link.c`)

```
collector ──HSFZ TCP 6801──▶ ZGW (gateway-ip.txt, default 169.254.21.3) ──▶ DME 0x12
      └─ fallback ──DSI──▶ DSYSMOSTServer ──MOST tunnel (fkt 0x050, op C)──▶ DME 0x12
```

- `--transport auto` (default): HSFZ if the gateway answers, else the MOST tunnel. `hsfz` needs `--host`, `most` ignores it.
- **This car [car] 2026-11-18:** no diagnostic gateway on ENET. HSFZ ident broadcast to UDP 6811 got no reply; the only other
  neighbour, `169.254.144.39` (MAC 00:13:1e…), has no 6801/6811/22/80. So `shift-deck-qnx` runs over **MOST**, and it works from hu-omap.
- MOST tunnel timing [car]: ~270 ms per request. One RPM + speed frame takes ~550 ms. A consumer's "stale" limit must be
  above that: ExtendedHUD uses 2 s (`drive_frame.h`'s 750 ms is for HSFZ).
- A collector exits when its parent dies (`getppid()`). `shift-deck-qnx` refuses to start without a parent.

## See the raw exchange

```bash
# SHIFTDECK_TRACE=1 in the environment prints the raw MOST exchange on stderr (opt-in; silent otherwise):
ssh root@169.254.199.99 'on -f hu-omap sh -c "export LD_LIBRARY_PATH=/lib:/usr/lib:/boot/lib:/opt/sys/lib SHIFTDECK_TRACE=1; \
  /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/shift-deck-qnx --host 169.254.21.3 --udp-port 42899 --cycles 60 2>&1"'
#   … p=f06371010f0b120004410c0000   →  41 0C 00 00 = 0 rpm (RPM = (A*256+B)/4);  …410d00 = 0 km/h
```

The tunnel reply payload is `f0 63 71 01 0f 0b <ecu 12> 00 <len> <uds reply…>`.

## Pitfalls (each one cost a car round trip)

- **`fork()` from a process with threads fails on QNX.** The HUD worker had opened MOST (DSI threads) before starting the collector, so the
  collector never ran. Use `posix_spawn` (C) or `spawn()` (C++, `diagnostic_worker.h`).
- **UDP ports are shared** by everything on hu-omap. If ShiftDeck is installed alongside ExtendedHUD, it binds `DRIVE_PORT` 42863 (and BoostDeck binds 42860). ExtendedHUD uses its own port **42873**. `on -f hu-omap sockstat` shows who holds which.
- `--cycles N` counts loop turns, including the ones before the link is up. A short run can end before MOST connects: use ≥ 60.
- Engine off with ignition on: the DME answers (0 rpm). Starting the engine is the owner's step ("start the engine now", parked, P, handbrake).
- Any new diagnostic PID/ECU read must stay read-only (`01`/`22`), record raw request and response, and be validated against an independent reading.
