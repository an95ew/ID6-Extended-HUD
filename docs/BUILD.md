# Build, ABI Verification, and Packaging

Run all commands from the repository root. Host tests require only a C compiler, Make, and Python 3 — no vehicle connection, QNX SDK, or BMW libraries are needed.

```sh
make -C native test
python3 -B -m unittest discover -s tools -p 'test_*.py'
python3 .claude/skills/extendedhud/check_app.py
```

## QNX SDP 6.5 Docker Toolchain

Native QNX ARM builds use QNX SDP 6.5 Linux x86 host tools with ARM GCC 4.4.2 (`qcc -V4.4.2,gcc_ntoarmv7le`), wrapped in Docker via `native/docker/qnx.sh`.

```sh
# One-time toolchain setup (stores the SDK in Docker volumes qnx650 and qnxetc):
sh native/docker/qnx.sh image
sh native/docker/qnx.sh install /path/to/folder-with-qnxsdp-6.5.0-dvd-iso
```

If QNX SDP 6.5 is installed natively on Linux, set `QNX_HOST`, `QNX_TARGET`, and `QNX_CONFIGURATION`, then invoke `sh native/build_hud.sh` or `sh native/build.sh` directly.

## Framework-Free Worker Build

`hud-sportbar-qnx` and `shift-deck-qnx` do not link against private BMW libraries (DSI is loaded at runtime via `dlopen`):

```sh
sh native/docker/qnx.sh build
```

Outputs in `native/out/` (git-ignored):
- `native/out/hud-sportbar-qnx` — HUDTerminal MOST worker
- `native/out/shift-deck-qnx` — read-only OBD `01 0C`/`01 0D` collector (HSFZ + [MOST fallback](MOST.md))

## Full Plugin Build and Standalone Package

Building `libExtendedHUD.so` requires copying the target head unit's own `libAppFramework.A.so` and `libCoreFramework.A.so` into `native/frameworks/` (git-ignored — never commit proprietary BMW binaries):

```sh
mkdir -p native/frameworks
# Standard NBT Evo Jacinto ENET IP is 169.254.199.99; substitute <HEAD_IP> if different.
# Single-node unit:
ssh root@<HEAD_IP> 'cat /opt/hmi/ID5/lib/libAppFramework.A.so' > native/frameworks/libAppFramework.A.so
ssh root@<HEAD_IP> 'cat /opt/hmi/ID5/lib/libCoreFramework.A.so' > native/frameworks/libCoreFramework.A.so
# Split-node unit (SSH lands on hu-jacinto; HMI resides on hu-omap over Qnet):
#   /net/hu-omap/fs/sda0/repository/istep/opt/hmi/ID5/lib/lib{App,Core}Framework.A.so
shasum -a 256 native/frameworks/*.so
```

Then build and assemble the self-contained plugin bundle and release archive:

```sh
sh native/docker/qnx.sh package
```

This runs `native/build.sh` and `native/package.sh`, verifies that every QNX binary is an ARM ELF32 executable/library, and writes:
- `native/out/ExtendedHUD/` — deployable plugin directory (`ExtendedHUD.manifest`, `ExtendedHUD.xml`, `libExtendedHUD.so`, `hud-sportbar-qnx`, `shift-deck-qnx`, `gateway-ip.txt`, `cds.txt`, `cameras.txt`)
- `native/out/extended-hud.zip` — archive containing `ExtendedHUD/`, `SHA256SUMS`, `LICENSE`, and `README.md`

For every release (versioning in [RELEASE-NOTES.md](../RELEASE-NOTES.md)): update `HUD_APP_VERSION` and `HUD_APP_RELEASED` in `native/src/hud/hud_app.h` (displayed on the iDrive **App information** page), add a section to `RELEASE-NOTES.md`, update the release line in `README.md` and `docs/EXTENDEDHUD.md`, and tag the commit `vX.Y.Z`. The speed-camera database is updated independently via `tools/cameras_build.py` (skill `update-speedcam-db`).

Where SDK runtime library versions differ, set `NBTEVO_RUNTIME_DIR` to a private directory containing matching `libm.so.2`, `libsocket.so.3`, and `libstdc++.so.6`, or set `NBTEVO_FRAMEWORKS_DIR` to point to an alternate frameworks folder.

## ABI Verification Before Installation

Every `bmw::` symbol imported by `libExtendedHUD.so` must be exported by the target head unit's framework pair — an unresolved symbol fails silently at `dlopen` inside `HmiMain3D`. Verify symbols using either the QNX SDK checker or the standalone pure-Python checker:

```sh
sh .claude/skills/deploy-plugins/abi_check.sh out/libExtendedHUD.so
python3 tools/abi_check.py native/out/libExtendedHUD.so native/frameworks/libAppFramework.A.so native/frameworks/libCoreFramework.A.so
```

To collect firmware version, screen resolution, and framework hashes from a target head unit read-only, use `tools/capture_headunit.py`. For deeper static vtable and symbol comparison between two framework pairs (requires `pyelftools` and `capstone`):

```sh
python3 native/tools/audit_framework_abi.py \
  --reference-dir /private/reference-frameworks \
  --installed-dir /private/candidate-frameworks \
  --plugin native/out/libExtendedHUD.so \
  --report /private/extendedhud-abi-report.json
```
