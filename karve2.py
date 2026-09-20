import re, struct, sys, os
data = open(sys.argv[1],'rb').read()
outdir = sys.argv[2]; os.makedirs(outdir, exist_ok=True)
FIELDS = ('name','type','flags','addr','offset','size','link','info','align','entsize')

def parse(off):
    if data[off:off+5] != b'\x7fELF\x01' or data[off+4] != 1: return None
    if struct.unpack_from('<H',data,off+16)[0] != 1: return None
    if struct.unpack_from('<H',data,off+18)[0] != 0x28: return None
    e_shoff,=struct.unpack_from('<I',data,off+32)
    e_shentsize,=struct.unpack_from('<H',data,off+46)
    e_shnum,=struct.unpack_from('<H',data,off+48)
    e_shstrndx,=struct.unpack_from('<H',data,off+50)
    if e_shentsize!=40 or not 0<e_shnum<400 or e_shstrndx>=e_shnum or e_shoff>(1<<25): return None
    w=struct.unpack('<%dI'%(e_shnum*10), data[off+e_shoff:off+e_shoff+e_shnum*40])
    secs=[dict(zip(FIELDS,w[i:i+10])) for i in range(0,len(w),10)]
    ss=secs[e_shstrndx]
    if ss['type']!=3: return None
    strtab=data[off+ss['offset']:off+ss['offset']+ss['size']]
    end=e_shoff+e_shnum*40
    names=[]
    for s in secs:
        if s['offset']>(1<<25) or s['size']>(1<<25): return None
        end=max(end,s['offset']+s['size']); names.append(None)
    body=data[off:off+end]
    for idx,s in enumerate(secs):
        z=strtab.find(b'\x00',s['name']); names[idx]=strtab[s['name']:z].decode('latin1')
    sm = {n:(s['offset'],s['size']) for s,n in zip(secs,names)}
    modname=None
    tm = sm.get('.gnu.linkonce.this_module') or sm.get('.modname')
    if tm:
        z=body.find(b'\x00',tm[0])
        cand=body[tm[0]:z].decode('latin1','replace')
        if re.fullmatch(r'[A-Za-z0-9_\-]{1,56}',cand): modname=cand
    if not modname:
        for s,n in zip(secs,names):
            if n=='__this_module' : pass
    m=re.search(rb'vermagic=([^\x00]{1,120})\x00',body)
    return dict(end=end, modname=modname, vermagic=m.group(1).decode('latin1') if m else None,
                body=body, sm=sm, sections=names)

res=[]
for m in re.finditer(rb'\x7fELF\x01', data):
    info=parse(m.start())
    if info: res.append((m.start(),info))
seen=set()
for off,info in res:
    nm=info['modname'] or ('anon_%x'%off)
    # skip overlapping duplicates (section headers pointing into same object)
    if any(abs(off-o)<64 for o in seen): continue
    seen.add(off)
    hasu = b'uwe5621' in info['body']
    print('%-28s off=%#x size=%-9d uwe=%d vermagic=%r' % (nm, off, info['end'], hasu, info['vermagic']))
    tag = nm
    if not re.fullmatch(r'[A-Za-z0-9_.\-]{1,64}', tag): tag='safe_%x'%off
    open(os.path.join(outdir, '%s.%#x.ko'.replace('%#x','%x')%(tag,off)),'wb').write(info['body'])
print('count', len(res), file=sys.stderr)
