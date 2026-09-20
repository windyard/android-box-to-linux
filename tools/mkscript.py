#!/usr/bin/env python3
"""Wrap a U-Boot text script in a legacy image header (mkimage -T script).

    mkscript.py script.txt out.bin [name]
"""
import struct, sys, time, zlib

IH_TYPE_SCRIPT = 14
MAGIC = 0x27051956


def main():
    src, dst = sys.argv[1], sys.argv[2]
    name = (sys.argv[3] if len(sys.argv) > 3 else "U-Boot script").encode()[:31]
    data = open(src, "rb").read()
    hdr = bytearray(64)
    struct.pack_into("!IIIIII", hdr, 0, MAGIC, int(time.time()), len(data),
                     0, 0, zlib.crc32(data) & 0xFFFFFFFF)
    hdr[24] = 0            # IH_OS_INVALID
    hdr[25] = 0            # IH_ARCH_INVALID
    hdr[26] = IH_TYPE_SCRIPT
    hdr[27] = 0            # IH_COMP_NONE
    hdr[28:60] = name + b"\0" * (32 - len(name))
    struct.pack_into("!I", hdr, 60, zlib.crc32(bytes(hdr[:60])) & 0xFFFFFFFF)
    open(dst, "wb").write(bytes(hdr) + data)
    print(f"{dst}: {64 + len(data)} bytes, script {len(data)} bytes, "
          f"ih_dcrc={zlib.crc32(data) & 0xFFFFFFFF:08x}")


if __name__ == "__main__":
    main()
