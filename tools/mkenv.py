#!/usr/bin/env python3
"""Rewrite the Amlogic U-Boot env image (magic + NUL-separated k=v, no CRC).

    mkenv.py in.bin out.bin key=value [key=value ...]

Existing keys keep their position and get a new value; unknown keys are
appended. Values are plain text (no NUL).
"""
import struct
import sys

MAGIC = b"\xe6\x8aYB"


def parse(data):
    assert data[:4] == MAGIC, "bad env magic %s" % data[:4].hex()
    pairs, pos = [], 4
    while pos < len(data):
        while pos < len(data) and data[pos] == 0:
            pos += 1
        if pos >= len(data):
            break
        end = data.find(b"\0", pos)
        if end < 0:
            end = len(data)
        tok = data[pos:end]
        key, _, val = tok.partition(b"=")
        pairs.append([key.decode("latin1"), val.decode("latin1")])
        pos = end + 1
    return pairs


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    size = len(data)
    pairs = parse(data)
    order = {k: i for i, (k, _) in enumerate(pairs)}
    for arg in sys.argv[3:]:
        key, _, val = arg.partition("=")
        if key in order:
            pairs[order[key]][1] = val
        else:
            order[key] = len(pairs)
            pairs.append([key, val])
    body = b"".join(("%s=%s" % (k, v)).encode("latin1") + b"\0" for k, v in pairs)
    if len(MAGIC) + len(body) > size:
        sys.exit("env overflow: %d > %d" % (len(MAGIC) + len(body), size))
    out = bytearray(MAGIC + body)
    out += bytes(size - len(out))
    open(dst, "wb").write(bytes(out))
    print("wrote %s: %d pairs, body %d bytes" % (dst, len(pairs), len(body)))


if __name__ == "__main__":
    main()
