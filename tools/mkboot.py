#!/usr/bin/env python3
"""Rebuild an Android v0/v1 boot image with a patched ramdisk directory.

    mkboot.py orig_boot.img extracted_ramdisk_dir out_boot.img

Kernel and second (dtb) are copied verbatim; the ramdisk is re-created as a
gzip cpio newc archive owned by root:root and ramdisk_size is updated.
"""
import gzip, struct, subprocess, sys, tempfile


def pages(sz, ps):
    return ((sz + ps - 1) // ps) * ps


def parse(buf):
    assert buf[:8] == b"ANDROID!", "not an Android boot image"
    f = struct.unpack_from("<8sIIIIIIIIII", buf, 0)
    d = {"magic": f[0], "kernel_size": f[1], "kernel_addr": f[2],
         "ramdisk_size": f[3], "ramdisk_addr": f[4], "second_size": f[5],
         "second_addr": f[6], "tags_addr": f[7], "page_size": f[8] or 2048,
         "header_version": f[9], "os_version": f[10]}
    assert d["header_version"] in (0, 1), f"unsupported header v{d['header_version']}"
    return d


def main():
    orig, rd_dir, out = sys.argv[1], sys.argv[2], sys.argv[3]
    buf = open(orig, "rb").read()
    d = parse(buf)
    ps = d["page_size"]
    name, cmdline = buf[48:64], buf[64:576]

    p = ps
    kernel = buf[p:p + d["kernel_size"]]
    p += pages(d["kernel_size"], ps)
    ramdisk_off = p
    p += pages(d["ramdisk_size"], ps)
    second = buf[p:p + d["second_size"]]
    tail = buf[p + pages(d["second_size"], ps):]
    if tail.strip(b"\0"):
        sys.exit("refusing: original image has non-zero data after the header layout")

    listing = subprocess.run(["find", "."], cwd=rd_dir,
                             stdout=subprocess.PIPE, check=True).stdout
    cpio = subprocess.run(["cpio", "-o", "-H", "newc", "--reproducible",
                           "--quiet", "-R", "0:0"], cwd=rd_dir,
                          input=listing, stdout=subprocess.PIPE, check=True).stdout
    print(f"cpio {len(cpio)} bytes")
    new_rdz = gzip.compress(cpio, 9, mtime=0)
    print(f"ramdisk {d['ramdisk_size']} -> {len(new_rdz)} bytes")

    hdr = bytearray(ps)
    struct.pack_into("<8sIIIIIIIIII", hdr, 0, d["magic"], d["kernel_size"],
                     d["kernel_addr"], len(new_rdz), d["ramdisk_addr"],
                     d["second_size"], d["second_addr"], d["tags_addr"], ps,
                     d["header_version"], d["os_version"])
    hdr[48:64] = name
    hdr[64:576] = cmdline

    blob = (bytes(hdr) + kernel
            + b"\0" * (pages(d["kernel_size"], ps) - d["kernel_size"])
            + new_rdz + b"\0" * (pages(len(new_rdz), ps) - len(new_rdz))
            + second + b"\0" * (pages(d["second_size"], ps) - d["second_size"]))
    open(out, "wb").write(blob)
    print(f"wrote {out}: {len(blob)} bytes (source partition dump {len(buf)})")


if __name__ == "__main__":
    main()
