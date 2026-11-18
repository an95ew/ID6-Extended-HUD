#!/bin/sh
# Assembles a standalone ExtendedHUD plugin bundle (out/ExtendedHUD/) and zip (out/extended-hud.zip)
# with SHA256SUMS, verifying every QNX binary is an ARM ELF32 file before packaging.
set -eu
cd "$(dirname "$0")"
python3 - <<'PY'
import hashlib
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import zipfile

NATIVE = Path.cwd()
OUT = NATIVE / "out"
PLUGIN = NATIVE / "plugin"
LICENSE = NATIVE.parent / "LICENSE"
THIRD_PARTY = NATIVE.parent / "third-party"
POLYFORM_NOTICE = THIRD_PARTY / "nbtevo-carplay-advanced-NOTICE"
IDRIVE7_LICENSE = THIRD_PARTY / "idrive7-apps-LICENSE"
STAGE = OUT / "ExtendedHUD"
ZIP_PATH = OUT / "extended-hud.zip"

REQUIRED_BINS = {
    "libExtendedHUD.so": (3,),      # ET_DYN shared library
    "hud-sportbar-qnx": (2,),       # ET_EXEC QNX worker
    "shift-deck-qnx": (2,),         # ET_EXEC QNX OBD collector
}
REQUIRED_CONFIG = ("ExtendedHUD.manifest", "ExtendedHUD.xml", "gateway-ip.txt")
EXECUTABLES = {"hud-sportbar-qnx", "shift-deck-qnx"}

def check_arm_elf(path: Path, allowed_types):
    if not path.is_file():
        sys.exit(f"Missing built binary: {path} (run native/docker/qnx.sh build-all first)")
    data = path.read_bytes()
    if len(data) < 20 or data[:6] != b"\x7fELF\x01\x01":
        sys.exit(f"{path}: not an ELF32 little-endian binary")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_machine != 40 or e_type not in allowed_types:
        sys.exit(f"{path}: expected ARM ELF type {allowed_types}, got e_type={e_type} e_machine={e_machine}")

for name, types in REQUIRED_BINS.items():
    check_arm_elf(OUT / name, types)
for name in REQUIRED_CONFIG:
    if not (PLUGIN / name).is_file():
        sys.exit(f"Missing plugin file: {PLUGIN / name}")
for extra in (LICENSE, POLYFORM_NOTICE, IDRIVE7_LICENSE):
    if not extra.is_file():
        sys.exit(f"Missing license/notice file: {extra}")

if STAGE.exists():
    shutil.rmtree(STAGE)
STAGE.mkdir(parents=True)

bundle_files = []
for name in REQUIRED_CONFIG:
    dst = STAGE / name
    shutil.copy2(PLUGIN / name, dst)
    dst.chmod(0o644)
    bundle_files.append((f"ExtendedHUD/{name}", dst, 0o644))

for name in REQUIRED_BINS:
    dst = STAGE / name
    shutil.copy2(OUT / name, dst)
    mode = 0o755 if name in EXECUTABLES else 0o644
    dst.chmod(mode)
    bundle_files.append((f"ExtendedHUD/{name}", dst, mode))

readme_text = (
    "# ExtendedHUD Package\n\n"
    "Standalone iDrive 6 (NBT Evo) HUD plugin folder built against the target car's\n"
    "libAppFramework.A.so and libCoreFramework.A.so.\n\n"
    "Verify checksums before installing:\n\n"
    "    shasum -a 256 -c SHA256SUMS\n\n"
    "Install ExtendedHUD/ into /var/opt/hmi/data/ID5/Plugins/ExtendedHUD/ on the HMI node,\n"
    "mark hud-sportbar-qnx and shift-deck-qnx executable (chmod 755), verify every uploaded\n"
    "file by hash readback, and restart iDrive. Do not install alongside HudTest.\n\n"
    "Licensing: MIT (LICENSE, LICENSE-idrive7-apps.txt), ISC (Mapbox glyph geometry in\n"
    "hud-sportbar-qnx), and PolyForm Noncommercial License 1.0.0\n"
    "(NOTICE-polyform-noncommercial.txt, applies to hud_rg compiled into hud-sportbar-qnx).\n"
)

sums_lines = []
for arcname, path, _ in sorted(bundle_files):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    sums_lines.append(f"{digest}  {arcname}")
sums_bytes = ("\n".join(sums_lines) + "\n").encode("utf-8")

def write_zip_bytes(zf: zipfile.ZipFile, arcname: str, data: bytes, mode: int):
    info = zipfile.ZipInfo(arcname)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (stat.S_IFREG | mode) << 16
    zf.writestr(info, data)

with zipfile.ZipFile(ZIP_PATH, "w") as zf:
    for arcname, path, mode in sorted(bundle_files):
        write_zip_bytes(zf, arcname, path.read_bytes(), mode)
    write_zip_bytes(zf, "SHA256SUMS", sums_bytes, 0o644)
    write_zip_bytes(zf, "LICENSE", LICENSE.read_bytes(), 0o644)
    write_zip_bytes(zf, "LICENSE-idrive7-apps.txt", IDRIVE7_LICENSE.read_bytes(), 0o644)
    write_zip_bytes(zf, "NOTICE-polyform-noncommercial.txt", POLYFORM_NOTICE.read_bytes(), 0o644)
    write_zip_bytes(zf, "README.md", readme_text.encode("utf-8"), 0o644)

# Read back and verify every entry in SHA256SUMS.
with zipfile.ZipFile(ZIP_PATH, "r") as zf:
    parsed = {}
    for line in zf.read("SHA256SUMS").decode("utf-8").splitlines():
        if line.strip():
            digest, name = line.split()
            parsed[name] = digest
    expected_names = {arcname for arcname, _, _ in bundle_files}
    if set(parsed) != expected_names:
        sys.exit(f"SHA256SUMS mismatch: expected {sorted(expected_names)}, got {sorted(parsed)}")
    for name, expected_digest in parsed.items():
        actual = hashlib.sha256(zf.read(name)).hexdigest()
        if actual != expected_digest:
            sys.exit(f"{name}: zip verification failed ({actual} != {expected_digest})")

print(f"Packaged {STAGE} and {ZIP_PATH} ({len(parsed)} verified files)")
print(sums_bytes.decode("utf-8").rstrip())
PY
