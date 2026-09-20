import re, struct, sys, os

img = sys.argv[1]; outdir = sys.argv[2]
filt = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
pat = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
os.makedirs(outdir, exist_ok=True)
data = open(img, 'rb').read()
SHT = {3:'strtab',2:'symtab'}
reasons = {}

def fail(r):
    reasons[r] = reasons.get(r,0)+1
    return None

def parse_at(off):
    if data[off:off+5] != b'\x7fELF\x01': return fail('magic')
    ei_class = data[off+4]
    if ei_class != 1: return fail('class%d'%ei_class)
    e_machine = struct.unpack_from('<H', data, off+18)[0]
    e_type    = struct.unpack_from('<H', data, off+16)[0]
    if e_machine != 0x28: return fail('machine')
    if e_type != 1: return fail('etype%d'%e_type)
    e_shoff    = struct.unpack_from('<I', data, off+32)[0]
    e_shentsize= struct.unpack_from('<H', data, off+46)[0]
    e_shnum    = struct.unpack_from('<H', data, off+48)[0]
    e_shstrndx = struct.unpack_from('<H', data, off+50)[0]
    if e_shentsize != 40: return fail('shentsize%d'%e_shentsize)
    if not (0 < e_shnum < 400): return fail('shnum%d'%e_shnum)
    if e_shstrndx >= e_shnum: return fail('shstrndx')
    if e_shoff > (1<<25): return fail('shoff')
    shdr_abs = off + e_shoff
    if shdr_abs + e_shnum*40 > len(data): return fail('shdr_truncated')
    raw = data[shdr_abs:shdr_abs+e_shnum*40]
    fields = ('name','type','flags','addr','offset','size','link','info','align','entsize')
    words = struct.unpack('<%dI' % (e_shnum*10), raw)
    secs = [dict(zip(fields, words[i:i+10])) for i in range(0, len(words), 10)]
    ss = secs[e_shstrndx]
    if ss['type'] != 3: return fail('shstrtab_type%d'%ss['type'])
    stro = off + ss['offset']
    if stro + ss['size'] > len(data) or ss['size'] < 2: return fail('shstrtab_range')
    strtab = data[stro:stro+ss['size']]
    names = []
    for s in secs:
        if s['name'] >= len(strtab): return fail('name_idx')
        z = strtab.find(b'\x00', s['name'])
        nm = strtab[s['name']:z].decode('latin1')
        if not re.fullmatch(r'[A-Za-z0-9_.\[\]$-]{0,64}', nm): return fail('name_char:'+nm[:20])
        names.append(nm)
    end = 0
    for s in secs:
        if s['offset'] > (1<<25) or s['size'] > (1<<25): return fail('sec_range')
        end = max(end, s['offset']+s['size'])
    if end > (1<<25): return fail('end')
    body = data[off:off+end]
    modname = None
    for s, nm in zip(secs, names):
        if nm in ('.modname', '.gnu.linkonce.this_module'):
            z = body.find(b'\x00', s['offset'])
            cand = body[s['offset']:z].decode('latin1')
            if re.fullmatch(r'[A-Za-z0-9_]{1,56}', cand):
                modname = cand
                if nm == '.modname': break
    ver = None
    m = re.search(rb'vermagic=([^\x00]{1,120})\x00', body)
    if m: ver = m.group(1).decode('latin1')
    return dict(size=end, modname=modname, vermagic=ver, nsec=e_shnum, body=body, names=names)

starts = [m.start() for m in re.finditer(rb'\x7fELF\x01', data)]
ok = 0
for st in starts:
    info = parse_at(st)
    if not info: continue
    ok += 1
    nm = info['modname'] or 'unnamed'
    print('%-34s off=%#x size=%-9d nsec=%-4d vermagic=%r' % (nm, st, info['size'], info['nsec'], info['vermagic']))
    if filt and not filt.search(nm): continue
    if not re.fullmatch(r'[A-Za-z0-9_.\-]{1,64}', nm): nm = 'safe_%x' % st
    open(os.path.join(outdir, nm+'.%.6x.ko'%st), 'wb').write(info['body'])
print('valid:', ok, file=sys.stderr)
print(reasons, file=sys.stderr)
