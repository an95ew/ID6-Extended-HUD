#!/bin/sh
# Builds the full ExtendedHUD QNX plugin suite against the car's own framework pair in frameworks/:
#   out/libExtendedHUD.so (HMI menu plugin)
#   out/hud-sportbar-qnx  (HUDTerminal MOST worker)
#   out/shift-deck-qnx    (read-only OBD RPM/speed collector)
#   out/librghook.so      (CarPlay route-guidance hook)
set -eu
cd "$(dirname "$0")"
export QNX_HOST="${QNX_HOST:-/opt/qnx650/host/linux/x86}"
export QNX_TARGET="${QNX_TARGET:-/opt/qnx650/target/qnx6}"
export QNX_CONFIGURATION="${QNX_CONFIGURATION:-/etc/qnx}"
frameworks="${NBTEVO_FRAMEWORKS_DIR:-$PWD/frameworks}"
runtime="${NBTEVO_RUNTIME_DIR:-$PWD/runtime}"
for lib in libAppFramework.A.so libCoreFramework.A.so; do
    test -f "$frameworks/$lib" || { echo "Missing private framework: $frameworks/$lib" >&2; exit 1; }
done
export PATH="$QNX_HOST/usr/bin:$PATH"
mkdir -p out
for pair in libm.so:libm.so.2 libsocket.so:libsocket.so.3 libstdc++.so:libstdc++.so.6; do
    name=${pair%:*}
    versioned=${pair#*:}
    if test -f "$runtime/$versioned"; then ln -sf "$runtime/$versioned" "out/$name"; fi
done
ntoarm-gcc -dumpversion
ntoarm-gcc -dumpmachine
c() { qcc -V4.4.2,gcc_ntoarmv7le -Wc,-std=c99 -Wall -Wextra -Werror "$@"; }
cxx() { qcc -V4.4.2,gcc_ntoarmv7le_gpp -lang-c++ -Wc,-std=gnu++98 -Wall -Wextra -Werror "$@"; }

c -Isrc -O2 -fPIC -c src/gauge_core.c -o out/core.o
for f in hud_app hud_scene hud_text; do
    c -Isrc -Isrc/hud -O2 -fPIC -c "src/hud/$f.c" -o "out/$f.o"
done
cxx -Isrc -Isdk -O2 -fno-strict-aliasing -fno-exceptions -fPIC -shared src/extended_hud.cpp out/core.o \
    out/hud_app.o out/hud_scene.o out/hud_text.o \
    "$frameworks/libAppFramework.A.so" "$frameworks/libCoreFramework.A.so" -Lout -lm -lsocket \
    -Wl,--allow-shlib-undefined -Wl,-soname,libExtendedHUD.so -o out/libExtendedHUD.so
shasum -a 256 out/libExtendedHUD.so 2>/dev/null || sha256sum out/libExtendedHUD.so

sh build_hud.sh
