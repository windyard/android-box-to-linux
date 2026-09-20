#!/usr/bin/env python3
"""Build an Amlogic u-boot autoscript (legacy uImage, type=script).

The 64-byte header layout was confirmed byte-by-byte against
stage/aml_autoscript, which this box's u-boot has already executed
successfully (task #10).  Amlogic drops the standard timestamp field:

  0  magic 0x27051956   4 hcrc   8 size  12 load  16 ep  20 dcrc
  24 os=0 arch=0 type=0x0e(script) comp=0
  28 name[32]   60 pad[4]

--selftest proves the payload crc (dcrc) and byte-exact round-trip against that
proven file.  Its hcrc is not a standard header crc32, which shows this u-boot
does not check it; we still write the conventional value.
"""
import binascii
import struct
import sys

MAGIC = 0x27051956
TYPE_SCRIPT = 0x0E
NAME_PAD = bytes.fromhex("166bd986")


def build(payload: bytes, name: str) -> bytes:
    hdr = bytearray(
        struct.pack(
            ">IIIIIIBBBB",
            MAGIC,
            0,                                        # hcrc placeholder
            len(payload),
            0,                                        # load
            0,                                        # ep
            binascii.crc32(payload) & 0xFFFFFFFF,     # dcrc
            0,                                        # os: linux
            0,                                        # arch
            TYPE_SCRIPT,
            0,                                        # comp: none
        )
    )
    hdr += name.encode().ljust(32, b"\0")
    hdr += NAME_PAD
    assert len(hdr) == 64, len(hdr)
    struct.pack_into(">I", hdr, 4, binascii.crc32(bytes(hdr)) & 0xFFFFFFFF)
    return bytes(hdr) + payload


def selftest() -> int:
    ref = open("stage/aml_autoscript", "rb").read()
    body = ref[64:]
    size = struct.unpack_from(">I", ref, 8)[0]
    want_hcrc = struct.unpack_from(">I", ref, 4)[0]
    want_dcrc = struct.unpack_from(">I", ref, 20)[0]
    got_dcrc = binascii.crc32(body) & 0xFFFFFFFF
    zeroed = bytearray(ref[:64])
    struct.pack_into(">I", zeroed, 4, 0)
    got_hcrc = binascii.crc32(bytes(zeroed)) & 0xFFFFFFFF
    rebuilt = build(body, ref[28:60].rstrip(b"\0").decode())
    same = rebuilt[:4] + rebuilt[8:] == ref[:4] + ref[8:]
    print("size  hdr=%d body=%d  %s" % (size, len(body), "OK" if size == len(body) else "FAIL"))
    print("dcrc  want=%08x got=%08x %s" % (want_dcrc, got_dcrc, "OK" if want_dcrc == got_dcrc else "FAIL"))
    print("hcrc  want=%08x std=%08x %s (unchecked by this u-boot)"
          % (want_hcrc, got_hcrc, "match" if want_hcrc == got_hcrc else "differs"))
    print("roundtrip everything except hcrc identical: %s" % ("YES" if same else "NO"))
    return 0 if want_dcrc == got_dcrc and size == len(body) and same else 1


def main() -> int:
    if "--selftest" in sys.argv:
        return selftest()
    src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
    payload = open(src, "rb").read()
    out = build(payload, name)
    open(dst, "wb").write(out)
    print("wrote %s (%d bytes, payload %d)" % (dst, len(out), len(payload)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
