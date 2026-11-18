# MOST fallback

On some cars the head unit has no route to the HSFZ diagnostic gateway (`169.254.21.3:6801`). ExtendedHUD's read-only OBD collector (`shift-deck-qnx`, built from `native/src/collector.c` and `native/src/diag_link.c`) then reaches the DME (`0x12`) through the head unit's own UDS tunnel over MOST.

## When the collector switches

- `shift-deck-qnx` is a loop worker (DSI client name `shiftdeck`). It switches from HSFZ to MOST after 5 failed HSFZ connects in a row.
- With no `--host`, it goes straight to MOST. ExtendedHUD passes the gateway address from `gateway-ip.txt` (default `169.254.21.3`), so on cars without an ENET gateway the switch happens automatically after the failed HSFZ connects.
- Once HSFZ connects in a process, that process stays on HSFZ.
- A failed MOST open is closed; the collector goes back to HSFZ and retries MOST after 30 s.
- Once on MOST, the collector stays there until restarted. If the tunnel drops, the collector exits and `hud-sportbar-qnx` restarts it.

The collector accepts `--transport auto|hsfz|most`. `auto` is the default, `hsfz` never uses MOST, and `most` ignores `--host`.

## What goes over MOST

- Only read-only OBD service `01` requests to DME `0x12`: PID `01 0C` (RPM) and `01 0D` (vehicle speed).
- Requests use routine `0x0F0B` on FBlock `0x06`, instance `0xF0`, FktID `0x050`, with diagnostic source `0x63`, through `DSYSMOSTServer`.
- A request waits the larger of its own timeout and 2 s; a pending reply extends that up to 5 s in all.
- On an F34 340i (`NBTevo_X20511I`), each request takes ~270 ms, so one RPM + speed frame takes ~550 ms. ExtendedHUD treats drive frames as stale after 2 s (`HUD_DRIVE_STALE_MS`).

## Sharing the tunnel

Diagnostic workers share the loopback diagnostics lease (`native/src/diagnostic_lease.h`) on both HSFZ and MOST. On MOST a loop worker holds the lease for one request at a time (waiting up to 300 ms) and yields before its next request if another worker is waiting.

## Library and limits

- The DSI library (`libsys_dsi_colibry...so`) is opened via `dlopen` only when falling back to MOST (first by name, then from `/opt/sys/lib`). Nothing links against proprietary DSI libraries. ExtendedHUD sets `LD_LIBRARY_PATH=/lib:/usr/lib:/boot/lib:/opt/sys/lib` when spawning its worker.
- Because a DSI send has no built-in timeout, `alarm()` bounds each loop iteration on MOST at 60 s and shutdown after `SIGTERM` at 15 s. On `SIGTERM` the collector unregisters its target and client cleanly before exiting.
- Set `SHIFTDECK_TRACE=1` in the environment to print the raw MOST exchange on `stderr`.

## Host tests

Host builds (`make -C native test`) use a fake DSI library selected by `ID6_FAKE_MOST`:

- unset: no library
- `up`: fake DME answering requests
- `drop`: link lost on the fifth request
- `no-server`: library loads, no server answers
- `mhd-refuse`: `up` with NRC `31` refusal
- `codes-512`, `codes-513`: reply-cap boundary checks in `test-diag-link`

`ID6_FAKE_MOST_DELAY_MS` (up to 2000) adds synthetic reply/close latency.

```sh
shift-deck-qnx --host 169.254.21.3 --udp-port 42873 --transport hsfz   # never MOST
SHIFTDECK_TRACE=1 shift-deck-qnx --udp-port 42899 --transport most     # MOST only + raw trace
shift-deck-qnx --host 169.254.21.3 --udp-port 42873                    # auto
```
