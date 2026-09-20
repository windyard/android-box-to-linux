#!/usr/bin/env python3
"""Find module vermagic strings in a raw image and attribute each to the nearest
preceding ARM ET_REL ELF header, so we can tell which .ko blobs are loadable."""
import re
import struct
import sys

path = sys.argv[1]
want = sys.argv[2] if len(sys.argv) > 2 else None
data = open(path, 'rb').read()
print(f'{path}: {len(data)} bytes')

elfs = [m.start() for m in re.finditer(rb'\x7fELF\x01', data)]
good = []
for o in elfs:
    if o + 52 > len(data):
        continue
    h = data[o:o + 52]
    e_type = struct.unpack('<H', h[16:18])[0]
    e_machine = struct.unpack('<H', h[18:20])[0]
    e_shoff = struct.unpack('<I', h[32:36])[0]
    e_shentsize = struct.unpack('<H', h[46:48])[0]
    e_shnum = struct.unpack('<H', h[48:50])[0]
    if e_machine == 0x28 and e_type == 1:
        good.append((o, e_shoff, e_shentsize, e_shnum))
import bisect
starts = [g[0] for g in good]
print(f'ARM ET_REL ELF candidates: {len(good)}')

seen = {}
for m in re.finditer(rb'vermagic=([^\x00]{1,80})\x00', data):
    vs = m.group(1).decode('ascii', 'replace')
    if want and want not in vs:
        continue
    o = m.start()
    i = bisect.bisect_right(starts, o) - 1
    owner = good[i][0] if i >= 0 else None
    span = (o - owner) if owner is not None else -1
    key = (owner, vs)
    if key in seen:
        continue
    seen[key] = 1
    if owner is not None and span < 8 * 1024 * 1024:
        print(f'  vermagic="{vs}"  elf@{hex(owner)} (+{span} B)')
    else:
        print(f'  vermagic="{vs}"  (loose string @{hex(o)})')
print('done')
