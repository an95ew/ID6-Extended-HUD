# MOST Diagnostic Tunnel Fallback

On some NBT Evo vehicles, the head unit has no direct IP route to the HSFZ diagnostic gateway (`169.254.21.3:6801`, default in `gateway-ip.txt`). ExtendedHUD's read-only OBD collector (`shift-deck-qnx`, built from `native/src/collector.c` and `native/src/diag_link.c`) automatically falls back to reaching the DME (`0x12`) through the head unit's own UDS tunnel over MOST.

## Transport Switching Behavior

- `shift-deck-qnx` runs as a loop worker (DSI client name `shiftdeck`). It falls back from HSFZ to MOST after 5 consecutive failed HSFZ connection attempts.
- When invoked without `--host`, it connects directly over MOST. ExtendedHUD passes the gateway IP from `gateway-ip.txt` (default `169.254.21.3`), so vehicles without a reachable ENET gateway switch to MOST automatically.
- Once an HSFZ connection succeeds, the collector process remains on HSFZ.
- If opening the MOST tunnel fails, the collector closes it, retries HSFZ, and attempts MOST again after 30 s.
- Once operating over MOST, the collector stays on MOST until exit. If the tunnel drops, the collector exits and `hud-sportbar-qnx` restarts it.

The collector accepts `--transport auto|hsfz|most`. `auto` is the default, `hsfz` disables MOST fallback, and `most` ignores `--host`.

## Protocol Details

- Strictly read-only OBD service `01` requests to DME `0x12`: PID `01 0C` (engine RPM) and `01 0D` (vehicle speed).
- Requests invoke routine `0x0F0B` on MOST FBlock `0x06`, instance `0xF0`, FktID `0x050`, using diagnostic source address `0x63` via `DSYSMOSTServer`.
- Each request waits the greater of its configured timeout and 2 s; a response-pending reply extends the wait up to 5 s total.
- On the reference test vehicle (2018 BMW F34 340i, B58, NBT Evo `NBTevo_X20511I`, split-node), each MOST request takes ~270 ms, yielding ~550 ms per RPM + speed pair. ExtendedHUD therefore treats drive frames as stale after 2 s (`HUD_DRIVE_STALE_MS`).

## Lease Sharing

Diagnostic workers coordinate access via the loopback diagnostic lease (`native/src/diagnostic_lease.h`) on both HSFZ and MOST. Over MOST, a loop worker holds the lease for a single request at a time (waiting up to 300 ms) and yields before its next request if another worker is queued.

## Runtime Library and Safety Limits

- The DSI shared library (`libsys_dsi_colibry*.so`) is loaded dynamically via `dlopen` only when falling back to MOST (searched by name first, then under `/opt/sys/lib`). No binary links against proprietary DSI libraries at build time. ExtendedHUD sets `LD_LIBRARY_PATH=/lib:/usr/lib:/boot/lib:/opt/sys/lib` when spawning its worker.
- Because DSI calls have no native timeout, `alarm()` bounds each MOST loop iteration at 60 s and shutdown on `SIGTERM` at 15 s. On `SIGTERM`, the collector unregisters its target and client cleanly before exiting.
- Set `SHIFTDECK_TRACE=1` in the environment to log raw MOST exchanges to `stderr`.

## Host Test Harness

Host test builds (`make -C native test`) use a mock DSI library controlled by `ID6_FAKE_MOST`:

- unset: library unavailable
- `up`: mock DME answering requests
- `drop`: link drops on the fifth request
- `no-server`: library loads, no server responds
- `mhd-refuse`: `up` with NRC `31` refusal
- `codes-512`, `codes-513`: reply-cap boundary tests in `test-diag-link`

`ID6_FAKE_MOST_DELAY_MS` (up to `2000`) injects synthetic reply/close latency.

```sh
shift-deck-qnx --host 169.254.21.3 --udp-port 42873 --transport hsfz   # HSFZ only
SHIFTDECK_TRACE=1 shift-deck-qnx --udp-port 42899 --transport most     # MOST only + raw trace
shift-deck-qnx --host 169.254.21.3 --udp-port 42873                    # automatic fallback
```
