#!/usr/bin/env python3
"""Identify and unpack an Amlogic/Android boot partition image.

Usage: unpack_aml.py image.img [outdir]
"""
import os, sys, struct, gzip

MAGIC_AB   = b"ANDROID!"
MAGIC_UIMG = b"\x27\x05\x19\x56"
MAGIC_GZIP = b"\x1f\x8b\x08"

def head(buf, off, n=32):
    return buf[off:off + n]

def parse_ab(buf, off):
    f = struct.unpack_from("<8sIIIIIIIII", buf, off)
    d = {"kernel_size": f[1], "kernel_addr": f[2], "ramdisk_size": f[3],
         "ramdisk_addr": f[4], "second_size": f[5], "second_addr": f[6],
         "tags_addr": f[7], "page_size": f[8]}
    d["header_version"] = struct.unpack_from("<I", buf, off + 40)[0]
    d["name"] = buf[off + 44:off + 76].split(b"\0")[0].decode("utf8", "replace")
    d["cmdline"] = buf[off + 76:off + 108].split(b"\0")[0].decode("utf8", "replace")
    return d

def cpio_entries(blob, limit=400):
    out, i = [], 0
    while len(out) < limit:
        j = blob.find(b"070701", i)
        if j == -1:
            break
        try:
            mode = int(blob[j + 46:j + 54], 16)
            filesize = int(blob[j + 54:j + 62], 16)
            namesize = int(blob[j + 94:j + 102], 16)
            nm = blob[j + 110:j + 110 + namesize - 1].decode("utf8", "replace")
        except Exception:
            break
        out.append((nm, oct(mode), filesize))
        i = j + 110 + ((namesize + 3) & ~3) + ((filesize + 3) & ~3)
    return out

def main():
    img = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else img + ".out"
    buf = open(img, "rb").read()
    print(f"file={img} size={len(buf)}")
    print("first64:", head(buf, 0, 64))

    off = buf.find(MAGIC_AB)
    if off == -1 or off > 65536:
        print(f"no ANDROID! magic (searched all: {off})")
        print("uImage" if buf[:4] == MAGIC_UIMG else "not uImage")
        if buf[:3] == MAGIC_GZIP:
            raw = gzip.decompress(buf[:1 << 26])
            print("gzip head:", head(raw, 0, 16))
            for nm, mode, sz in cpio_entries(raw, 30):
                print(f"  {mode} {sz:>9} {nm}")
        return 1

    print(f"ANDROID! at offset {off} ({off} bytes of vendor prefix)")
    d = parse_ab(buf, off)
    print("header:", d)
    ps = d["page_size"] or 2048
    os.makedirs(outdir, exist_ok=True)

    p = off + ps
    kernel = buf[p:p + d["kernel_size"]]
    open(outdir + "/kernel", "wb").write(kernel)
    print("kernel", len(kernel), head(kernel, 0, 16))

    p += ((d["kernel_size"] + ps - 1) // ps) * ps
    ramdisk = buf[p:p + d["ramdisk_size"]]
    open(outdir + "/ramdisk", "wb").write(ramdisk)
    print("ramdisk", len(ramdisk), head(ramdisk, 0, 16))

    if ramdisk[:3] == MAGIC_GZIP:
        raw = gzip.decompress(ramdisk)
        open(outdir + "/ramdisk.cpio", "wb").write(raw)
        ents = cpio_entries(raw)
        print(f"ramdisk cpio: {len(ents)} entries (first 40)")
        for nm, mode, sz in ents[:40]:
            print(f"  {mode} {sz:>9} {nm}")
    elif ramdisk[:6] == b"070701":
        open(outdir + "/ramdisk.cpio", "wb").write(ramdisk)
        ents = cpio_entries(ramdisk)
        print(f"raw cpio ramdisk: {len(ents)} entries (first 40)")
        for nm, mode, sz in ents[:40]:
            print(f"  {mode} {sz:>9} {nm}")

    if d["second_size"]:
        p += ((d["ramdisk_size"] + ps - 1) // ps) * ps
        open(outdir + "/second", "wb").write(buf[p:p + d["second_size"]])
        print("second/dtb", d["second_size"], "at", p, head(buf, p, 8))

    print("tail32:", head(buf, len(buf) - 32, 32))
    return 0

if __name__ == "__main__":
    sys.exit(main())
