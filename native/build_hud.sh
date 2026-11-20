#!/bin/sh
# Builds the framework-free ExtendedHUD QNX binaries:
#   out/hud-sportbar-qnx  (HUDTerminal MOST 82/02 worker)
#   out/shift-deck-qnx    (read-only OBD 01 0C / 01 0D DME collector)
# None of these link private BMW frameworks; the DSI library is opened at run time.
set -eu
cd "$(dirname "$0")"
export QNX_HOST="${QNX_HOST:-/opt/qnx650/host/linux/x86}"
export QNX_TARGET="${QNX_TARGET:-/opt/qnx650/target/qnx6}"
export QNX_CONFIGURATION="${QNX_CONFIGURATION:-/etc/qnx}"
export PATH="$QNX_HOST/usr/bin:$PATH"
mkdir -p out
c() { qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=c99 -Wall -Wextra -Werror "$@"; }

c -Isrc -Isrc/hud -O2 \
    src/hud/hud_worker.c src/hud/hud_most.c src/hud/hud_proto.c src/hud/hud_strip.c src/hud/hud_scene.c \
    src/hud/hud_sender.c src/hud/hud_gear.c src/hud/hud_cam.c \
    src/most/*.c -lm -lsocket -o out/hud-sportbar-qnx

c -Isrc -O2 \
    src/collector.c src/gauge_core.c src/diag_link.c src/most/*.c -Lout -lm -lsocket -o out/shift-deck-qnx

for f in out/hud-sportbar-qnx out/shift-deck-qnx; do
    shasum -a 256 "$f" 2>/dev/null || sha256sum "$f"
done
