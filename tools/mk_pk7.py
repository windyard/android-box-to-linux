import hashlib, struct
from cryptography.hazmat.primitives import serialization
from cryptography import x509

def der(tag, content):
    if len(content) < 128:
        return bytes([tag, len(content)]) + content
    lb = len(content).to_bytes((len(content).bit_length()+7)//8, 'big')
    return bytes([tag, 0x80 | len(lb)]) + lb + content

def OID(s):
    parts = [int(x) for x in s.split('.')]
    out = bytearray([parts[0]*40 + parts[1]])
    for p in parts[2:]:
        chunks = []
        while True:
            chunks.insert(0, p & 0x7f)
            p >>= 7
            if p == 0:
                break
        for i, c in enumerate(chunks):
            out.append(c | (0x80 if i < len(chunks)-1 else 0))
    return bytes(out)

def INTEGER(v):
    return der(0x02, v.to_bytes((v.bit_length()+8)//8, 'big'))

def SEQ(*items): return der(0x30, b''.join(items))
def SET(*items): return der(0x31, b''.join(items))
def ctx(n, content): return der(0xA0|n, content)

def tlv(data, off=0):
    tag = data[off]; off += 1
    ln = data[off]; off += 1
    if ln & 0x80:
        nb = ln & 0x7f
        ln = int.from_bytes(data[off:off+nb],'big'); off += nb
    return tag, data[off:off+ln], off+ln

key = serialization.load_pem_private_key(open('/tmp/testkey.key.pem','rb').read(), None)
pub = key.public_key().public_numbers()
N, D, E = pub.n, key.private_numbers().d, pub.e

oid_sha1_tl = der(0x06, OID('1.3.14.3.2.26'))
oid_rsa_tl  = der(0x06, OID('1.2.840.113549.1.1.1'))
oid_sd_tl   = der(0x06, OID('1.2.840.113549.1.7.2'))
oid_data_tl = der(0x06, OID('1.2.840.113549.1.7.1'))
oid_ct_tl   = der(0x06, OID('1.2.840.113549.1.9.3'))
oid_md_tl   = der(0x06, OID('1.2.840.113549.1.9.4'))
NULL = der(0x05, b'')
alg_sha1 = SEQ(oid_sha1_tl, NULL)
alg_rsa  = SEQ(oid_rsa_tl, NULL)

crt = x509.load_pem_x509_certificate(open('/tmp/testkey.x509.pem','rb').read())
cert_der = crt.public_bytes(serialization.Encoding.DER)
_, outer, _ = tlv(cert_der)
_, tbs, _ = tlv(outer)
p = 0
_, _, p = tlv(tbs, p)   # [0] version
_, serial, p = tlv(tbs, p)
_, _, p = tlv(tbs, p)   # sigalg
_, issuer, p = tlv(tbs, p)
issuer_der = der(0x30, issuer)
serial_der = der(0x02, serial)

base = open('/tmp/base.zip','rb').read()
i_eocd = base.rfind(b'PK\x05\x06')
assert base[i_eocd+22:] == b"", i_eocd
signed = base[:i_eocd+20]
sha1 = hashlib.sha1(signed).digest()

DIGINFO = bytes.fromhex('3021300906052b0e03021a05000414')
di = DIGINFO + sha1
el = 256
em = b'\x00\x01' + b'\xff'*(el-len(di)-3) + b'\x00' + di
sig_raw = pow(int.from_bytes(em,'big'), D, N).to_bytes(el, 'big')

attr_ct = SEQ(oid_ct_tl, SET(oid_sd_tl))
attr_md = SEQ(oid_md_tl, SET(der(0x04, sha1)))
authattrs = ctx(0, attr_ct + attr_md)

signer_info = SEQ(INTEGER(1), SEQ(issuer_der + serial_der), alg_sha1, alg_rsa, der(0x04, sig_raw))
signed_data = SEQ(INTEGER(1), SET(alg_sha1), SEQ(oid_data_tl), ctx(0, cert_der), SET(signer_info))
content_info = SEQ(oid_sd_tl, ctx(0, signed_data))

sig_block = content_info
cs = len(sig_block) + 6
final = signed + struct.pack('<H', cs) + sig_block + struct.pack('<H', cs) + b'\xff\xff' + struct.pack('<H', cs)


# ---- faithful port of asn1_context.cpp ----
class Ctx:
    def __init__(self, buf): self.p = 0; self.buf = buf
    def peek(self): return self.buf[self.p] if self.p < len(self.buf) else -1
    def getb(self):
        if self.p >= len(self.buf): return -1
        b = self.buf[self.p]; self.p += 1; return b
    def skip(self, n):
        if self.p + n > len(self.buf): return False
        self.p += n; return True
    def dec_len(self):
        t = self.getb()
        if t == -1: return None
        if t & 0x80 == 0: return t
        nb = t & 0x1f
        v = 0
        for _ in range(nb):
            b = self.getb()
            if b == -1: return None
            v = (v << 8) + b
        return v
    def constructed_get(self):
        t = self.getb()
        if t == -1 or (t & 0xE0) != 0xA0: return None
        ln = self.dec_len()
        if ln is None or ln > len(self.buf) - self.p: return None
        return Ctx(self.buf[self.p:self.p+ln])
    def constructed_skip_all(self):
        b = self.peek()
        while b != -1 and (b & 0xE0) == 0xA0:
            self.getb()
            ln = self.dec_len()
            if ln is None or not self.skip(ln): return False
            b = self.peek()
        return b != -1
    def sequence_get(self):
        t = self.getb()
        if t == -1 or (t & 0x7F) != 0x30: return None
        ln = self.dec_len()
        if ln is None or ln > len(self.buf) - self.p: return None
        return Ctx(self.buf[self.p:self.p+ln])
    def set_get(self):
        t = self.getb()
        if t == -1 or (t & 0x7F) != 0x31: return None
        ln = self.dec_len()
        if ln is None or ln > len(self.buf) - self.p: return None
        return Ctx(self.buf[self.p:self.p+ln])
    def sequence_next(self):
        t = self.getb()
        if t == -1: return False
        ln = self.dec_len()
        if ln is None: return False
        return self.skip(ln)
    def octet_get(self):
        if self.getb() != 0x04: return None
        ln = self.dec_len()
        if ln is None or ln == 0 or ln > len(self.buf) - self.p: return None
        return self.buf[self.p:self.p+ln]

ctx = Ctx(sig_block)
seq = ctx.sequence_get(); assert seq
assert seq.sequence_next()
app = seq.constructed_get(); assert app
sd = app.sequence_get(); assert sd
assert sd.sequence_next() and sd.sequence_next() and sd.sequence_next()
assert sd.constructed_skip_all()
ss = sd.set_get(); assert ss
si = ss.sequence_get(); assert si
for _ in range(4): assert si.sequence_next()
extracted = si.octet_get()
assert extracted == sig_raw, 'octet mismatch'
m = pow(int.from_bytes(extracted,'big'), E, N).to_bytes(el,'big')
assert m[:2] == b'\x00\x01'
sep = m.find(b'\x00', 2)
assert m[sep+1:] == DIGINFO + hashlib.sha1(signed).digest()
open('/tmp/pk7.zip','wb').write(final)
print('ALL CHECKS PASS', len(final), hashlib.md5(final).hexdigest().upper())
