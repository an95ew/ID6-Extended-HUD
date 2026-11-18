#!/bin/sh
# Checks that every bmw:: symbol a plugin library needs is defined by THIS car's framework pair, before it goes on the car.
# An unresolved symbol only fails at dlopen inside HmiMain3D (the app silently never appears). Uses the QNX Docker toolchain.
#
#   sh .claude/skills/deploy-plugins/abi_check.sh [LIB.so ...]     # default: every native/out/lib*.so
#
# Needs native/frameworks/lib{App,Core}Framework.A.so copied read-only from the car (git-ignored; see SKILL.md).
# Exit status 1 if any library misses a symbol.
set -eu
cd "$(dirname "$0")/../../../native"
[ -f frameworks/libAppFramework.A.so ] && [ -f frameworks/libCoreFramework.A.so ] || { echo "frameworks/ missing: copy them from the car first" >&2; exit 2; }
if [ $# -eq 0 ]; then set -- out/lib*.so; fi
libs=""
for l in "$@"; do libs="$libs ${l#native/}"; done
docker run --rm -i --platform linux/amd64 -v qnx650:/opt/qnx650 -v qnxetc:/etc/qnx -v "$PWD":/src -w /src qnx650-host sh -c '
NM=/opt/qnx650/host/linux/x86/usr/bin/ntoarm-nm
$NM -D --defined-only frameworks/libAppFramework.A.so frameworks/libCoreFramework.A.so | awk "{print \$NF}" | sort -u > /tmp/def
bad=0
for l in '"$libs"'; do
  $NM -D --undefined-only "$l" | awk "{print \$NF}" | grep 3bmw | sort -u > /tmp/und || true
  miss=$(comm -23 /tmp/und /tmp/def)
  info=$($NM -D --defined-only "$l" | grep -c " pluginAppInfo$" || true)
  if [ -n "$miss" ]; then echo "$l: MISSING $miss"; bad=1; else echo "$l: $(wc -l < /tmp/und) bmw symbols ok, pluginAppInfo=$info"; fi
done
exit $bad'
