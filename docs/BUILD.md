# Build, ABI verification, and packaging

Run commands from the repository root. Host tests need only a C compiler, Make, and Python 3 — no car, QNX SDK, or BMW files are required.

```sh
make -C native test
python3 -B -m unittest discover -s tools -p 'test_*.py'
python3 .claude/skills/extendedhud/check_app.py
```

## QNX SDP 6.5 Docker toolchain

Native QNX ARM builds use QNX SDP 6.5 Linux x86 host tools with ARM GCC 4.4.2 (`qcc -V4.4.2,gcc_ntoarmv7le`), wrapped in Docker via `native/docker/qnx.sh`.

```sh
# One-time toolchain setup (stores the SDK in Docker volumes qnx650 and qnxetc):
sh native/docker/qnx.sh image
sh native/docker/qnx.sh install /path/to/folder-with-qnxsdp-6.5.0-dvd-iso
```

If you have QNX SDP 6.5 installed natively on Linux, set `QNX_HOST`, `QNX_TARGET`, and `QNX_CONFIGURATION` and invoke `sh native/build_hud.sh` or `sh native/build.sh` directly.

## Framework-free worker build

`hud-sportbar-qnx`, `shift-deck-qnx`, and `librghook.so` link no private BMW libraries (DSI is loaded at run time with `dlopen`):

```sh
sh native/docker/qnx.sh build
```

Outputs in `native/out/` (git-ignored):
- `native/out/hud-sportbar-qnx` — HUDTerminal MOST worker
- `native/out/shift-deck-qnx` — read-only OBD `01 0C`/`01 0D` collector (HSFZ + [MOST fallback](MOST.md))
- `native/out/librghook.so` — CarPlay route-guidance hook (built only, not installed by default)

## Full plugin build and standalone package

Building `libExtendedHUD.so` requires copying the target car's own `libAppFramework.A.so` and `libCoreFramework.A.so` into `native/frameworks/` (git-ignored — never commit proprietary BMW binaries):

```sh
mkdir -p native/frameworks
# Single-node unit:
ssh root@HEAD_IP 'cat /opt/hmi/ID5/lib/libAppFramework.A.so' > native/frameworks/libAppFramework.A.so
ssh root@HEAD_IP 'cat /opt/hmi/ID5/lib/libCoreFramework.A.so' > native/frameworks/libCoreFramework.A.so
# Split-node unit (SSH lands on hu-jacinto; HMI lives on hu-omap over Qnet):
#   /net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib/lib{App,Core}Framework.A.so
shasum -a 256 native/frameworks/*.so
```

Then build and assemble the self-contained plugin bundle and zip:

```sh
sh native/docker/qnx.sh package
```

This runs `native/build.sh` and `native/package.sh`, verifying every binary is an ARM ELF32 file and writing:
- `native/out/ExtendedHUD/` — ready-to-deploy plugin folder (`ExtendedHUD.manifest`, `ExtendedHUD.xml`, `libExtendedHUD.so`, `hud-sportbar-qnx`, `shift-deck-qnx`, `gateway-ip.txt`)
- `native/out/extended-hud.zip` — archive with `ExtendedHUD/`, `SHA256SUMS`, `LICENSE`, and `README.md`

Where SDK linker versions differ, set `NBTEVO_RUNTIME_DIR` to a private directory containing matching `libm.so.2`, `libsocket.so.3`, and `libstdc++.so.6`, or `NBTEVO_FRAMEWORKS_DIR` to point to an alternate frameworks folder.

## ABI verification before installing

Every `bmw::` symbol imported by `libExtendedHUD.so` must be exported by the target car's framework pair — an unresolved symbol fails silently at `dlopen` inside `HmiMain3D`:

```sh
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so
```

To collect firmware, screen resolution, and framework hashes from a new head unit read-only, use `tools/capture_headunit.py`. For deeper static vtable/symbol comparison between two framework pairs (requires `pyelftools` and `capstone`):

```sh
python3 native/tools/audit_framework_abi.py \
  --reference-dir /private/reference-frameworks \
  --installed-dir /private/candidate-frameworks \
  --plugin native/out/libExtendedHUD.so \
  --report /private/extendedhud-abi-report.json
```
