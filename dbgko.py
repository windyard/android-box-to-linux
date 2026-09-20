import re, struct, sys
data = open(sys.argv[1],'rb').read()
lim = int(sys.argv[2]) if len(sys.argv)>2 else len(data)
bad = {}
n=0
for m in re.finditer(rb'\x7fELF\x01', data):
    off = m.start()
    if off+52 > len(data): continue
    e_machine = struct.unpack_from('<H', data, off+18)[0]
    e_type = struct.unpack_from('<H', data, off+16)[0]
    if e_machine != 0x28 or e_type != 1: continue
    n+=1
    if n>lim: break
    e_shoff = struct.unpack_from('<I', data, off+32)[0]
    e_shentsize = struct.unpack_from('<H', data, off+46)[0]
    e_shnum = struct.unpack_from('<H', data, off+48)[0]
    e_shstrndx = struct.unpack_from('<H', data, off+50)[0]
    key = (e_shentsize, e_shnum<2000)
    bad[key] = bad.get(key,0)+1
    if bad[key] <= 2:
        print('off=%#x shoff=%#x shentsize=%d shnum=%d shstrndx=%d' % (off, e_shoff, e_shentsize, e_shnum, e_shstrndx))
print('candidates:', n)
print(bad)
