#!/bin/sh
# QNX SDP 6.5 in Docker. The toolchain lives in the volumes qnx650 (/opt/qnx650) and qnxetc (/etc/qnx).
#   qnx.sh image                  build the container image
#   qnx.sh install DIR_WITH_ISO   run the official installer in console mode (asks for the licence key)
#   qnx.sh build                  run build_hud.sh -> out/hud-sportbar-qnx, out/shift-deck-qnx
#   qnx.sh build-all              run build.sh -> adds out/libExtendedHUD.so (needs native/frameworks/)
#   qnx.sh package                run build.sh + package.sh -> out/ExtendedHUD/ and out/extended-hud.zip
#   qnx.sh shell                  shell with the toolchain mounted
set -eu
cd "$(dirname "$0")/.."
IMG=qnx650-host
TTY=-i; [ -t 0 ] && TTY=-it
RUN="docker run --rm $TTY --platform linux/amd64 -v qnx650:/opt/qnx650 -v qnxetc:/etc/qnx"

case "${1:-}" in
image)
    docker build --platform linux/amd64 -t "$IMG" docker ;;
install)
    dir=$(cd "${2:?folder with the SDP 6.5 ISO}" && pwd)
    iso=$(ls "$dir"/qnxsdp-6.5.0-*-dvd.iso | head -1)
    # Docker Desktop can't share a mounted disk image, so unpack the disc once into a plain folder.
    if [ ! -f "$dir/disc/.unpacked" ]; then
        rm -rf "$dir/disc" && mkdir "$dir/disc"
        echo "unpacking $(basename "$iso") to $dir/disc ..."
        bsdtar -xf "$iso" -C "$dir/disc"
        chmod -R u+w "$dir/disc" && touch "$dir/disc/.unpacked"
    fi
    # InstallShield: run from the disc so it finds qnx_linux_setup.jar; install to the default /opt/qnx650.
    $RUN -v "$dir/disc":/iso:ro "$IMG" sh -c 'cd /iso && ./qnxsdp-6.5.0-*-linux.bin -console -is:tempdir /tmp' ;;
build)
    $RUN -v "$PWD":/src "$IMG" sh build_hud.sh ;;
build-all)
    $RUN -v "$PWD":/src "$IMG" sh build.sh ;;
package)
    $RUN -v "$PWD":/src "$IMG" sh build.sh
    sh package.sh ;;
shell)
    $RUN -v "$PWD":/src "$IMG" bash ;;
*)
    sed -n '2,8p' "$0"; exit 2 ;;
esac
