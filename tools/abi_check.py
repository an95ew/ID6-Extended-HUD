#!/usr/bin/env python3
"""Does libExtendedHUD.so fit this head unit? Pure Python (no QNX SDK, no pip packages), read-only.

Every bmw:: symbol the plugin library needs must be defined by the car's own framework pair, or HmiMain3D's dlopen fails
and the app silently never appears in the menu. Same check as .claude/skills/deploy-plugins/abi_check.sh (ntoarm-nm), for
people who install a prebuilt package (INSTALL.md).

    python3 tools/abi_check.py LIB.so libAppFramework.A.so libCoreFramework.A.so

Copy the two frameworks from the car first (INSTALL.md); never commit or share them. Exit 0 when every symbol resolves,
1 when some are missing (they are printed), 2 on a bad file.
"""
import struct
import sys


def dynamic_symbols(path):
    """(defined, undefined) symbol names of an ELF32 little-endian file's .dynsym."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("%s: not an ELF32 little-endian file" % path)
    e_shoff, = struct.unpack_from("<I", data, 32)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 46)
    sections = [struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    defined, undefined = set(), set()
    for sh in sections:
        sh_type, sh_offset, sh_size, sh_link, sh_entsize = sh[1], sh[4], sh[5], sh[6], sh[9] or 16
        if sh_type != 11:   # SHT_DYNSYM
            continue
        str_off = sections[sh_link][4]
        for k in range(sh_size // sh_entsize):
            st_name, _, _, _, _, st_shndx = struct.unpack_from("<IIIBBH", data, sh_offset + k * sh_entsize)
            if not st_name:
                continue
            start = str_off + st_name
            name = data[start:data.index(b"\0", start)].decode("ascii", "replace")
            (undefined if st_shndx == 0 else defined).add(name)
    return defined, undefined


def main(argv):
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        plugin_defined, need = dynamic_symbols(argv[1])
        have = set()
        for fw in argv[2:]:
            have |= dynamic_symbols(fw)[0]
    except (OSError, ValueError, struct.error) as e:
        print("abi_check: %s" % e, file=sys.stderr)
        return 2
    bmw = sorted(s for s in need if "3bmw" in s)
    missing = [s for s in bmw if s not in have]
    if missing:
        print("%s: %d of %d bmw symbols MISSING in these frameworks:" % (argv[1], len(missing), len(bmw)))
        for s in missing:
            print("  " + s)
        return 1
    info = "pluginAppInfo" in plugin_defined
    print("%s: %d bmw symbols ok, pluginAppInfo=%d" % (argv[1], len(bmw), 1 if info else 0))
    return 0 if info else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
