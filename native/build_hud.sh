#!/bin/sh
# Builds the framework-free ExtendedHUD QNX binaries:
#   out/hud-sportbar-qnx  (HUDTerminal MOST 82/02 worker)
#   out/shift-deck-qnx    (read-only OBD 01 0C / 01 0D DME collector)
#   out/librghook.so      (CarPlay route-guidance hook for mm-ipod)
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
    src/hud/hud_sender.c src/hud/hud_nav.c src/hud/hud_nav_demo.c src/hud/hud_text.c src/hud/hud_rg.c \
    src/most/*.c -lm -lsocket -o out/hud-sportbar-qnx

c -Isrc -O2 -DNATIVE_FAST \
    src/collector.c src/gauge_core.c src/diag_link.c src/most/*.c -Lout -lm -lsocket -o out/shift-deck-qnx

# librghook.so (CarPlay route guidance, preloaded into mm-ipod; see src/rghook/rghook.c).
# Built only: nothing here installs it.
c -O2 -fPIC -shared -Wl,-soname,librghook.so \
    src/rghook/rghook.c -o out/librghook.so

for f in out/hud-sportbar-qnx out/shift-deck-qnx out/librghook.so; do
    shasum -a 256 "$f" 2>/dev/null || sha256sum "$f"
done
