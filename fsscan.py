import sys, re
img = sys.argv[1]
target = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x3EC6F7F8
pats = {
 'squashfs': rb'hsqs',
 'erofs':    rb'\xe2\xe5\xf0\xe0',
 'ext4@56':  rb'S\353\354\367',
 'yaffs2':   rb'^\x00\x00\x00',
}
data = open(img,'rb').read()
print('size', len(data), 'target', hex(target))
for name, p in pats.items():
    hits = [m.start() for m in re.finditer(re.escape(p), data)]
    print(name, 'hits:', len(hits))
    near = [h for h in hits if abs(h-target) < (64<<20)]
    print('   near target:', [hex(h) for h in near[:10]])
    if name=='squashfs':
        for h in hits[:40]: print('   sqsh', hex(h), 'size field', int.from_bytes(data[h+20:h+24],'little') if data[h:h+4]==b'hsqs' else '?')
