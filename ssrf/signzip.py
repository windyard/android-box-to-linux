#!/usr/bin/env python3
"""Create a recovery-compatible update.zip signed with the AOSP testkey (JAR/CMS v1)."""
import base64, hashlib, os, sys, zipfile
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding as asym_padding
from cryptography import x509
from cryptography.hazmat.primitives.serialization import pkcs7

KEY = "/tmp/testkey.key.pem"
CRT = "/tmp/testkey.x509.pem"

def b64(data):
    return base64.b64encode(data).decode()

def manifest_sha256(entries):
    lines = ["Manifest-Version: 1.0", "Created-By: 1.0 (Android)"]
    per_entry = []
    for name in entries:
        data = entries[name]
        per_entry += [f"Name: {name}",
                      f"SHA1-Digest: {b64(hashlib.sha1(data).digest())}",
                      f"SHA-256-Digest: {b64(hashlib.sha256(data).digest())}", ""]
    body = ("\r\n".join(lines) + "\r\n\r\n" + "\r\n".join(per_entry) + "\r\n").encode()
    return body

def sf(manifest_bytes, entries):
    lines = [
        "Signature-Version: 1.0",
        f"SHA1-Digest-Manifest: {b64(hashlib.sha1(manifest_bytes).digest())}",
        f"SHA-256-Digest-Manifest: {b64(hashlib.sha256(manifest_bytes).digest())}",
        "Created-By: 1.0 (Android)",
    ]
    per_entry = []
    for name in entries:
        # exact per-entry block from manifest, INCLUDING the terminating blank line
        idx = manifest_bytes.find(b"Name: " + name.encode() + b"\r\n")
        end = manifest_bytes.find(b"\r\n\r\n", idx)
        block = manifest_bytes[idx:end + 4]
        per_entry += [f"Name: {name}",
                      f"SHA1-Digest: {b64(hashlib.sha1(block).digest())}",
                      f"SHA-256-Digest: {b64(hashlib.sha256(block).digest())}", ""]
    body = ("\r\n".join(lines) + "\r\n\r\n" + "\r\n".join(per_entry) + "\r\n").encode()
    return body

def pkcs7_der(content: bytes):
    key = serialization.load_pem_private_key(open(KEY, "rb").read(), password=None)
    cert = x509.load_pem_x509_certificate(open(CRT, "rb").read())
    from cryptography.hazmat.backends import default_backend
    # build with empty payload file then detached
    import tempfile
    with tempfile.NamedTemporaryFile(delete=False) as tf:
        tf.write(content); p = tf.name
    try:
        der = (
            pkcs7.PKCS7SignatureBuilder()
            .set_data(content)
            .add_signer(cert, key, hashes.SHA256())
            .sign(serialization.Encoding.DER,
                  [pkcs7.PKCS7Options.DetachedSignature,
                   pkcs7.PKCS7Options.Binary,
                   pkcs7.PKCS7Options.NoCapabilities])
        )
    finally:
        os.unlink(p)
    return der

def build(out_path, payload_entries):
    entries = dict(payload_entries)
    names = sorted(entries)
    mf = manifest_sha256(entries)
    certsf = sf(mf, entries)
    certrsa = pkcs7_der(certsf)
    extra = {"META-INF/MANIFEST.MF": mf, "META-INF/CERT.SF": certsf, "META-INF/CERT.RSA": certrsa}
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        for n in names:
            zi = zipfile.ZipInfo(n, date_time=(2019, 1, 1, 0, 0, 0))
            zi.external_attr = 0o755 << 16 if n.endswith("update-binary") else 0o644 << 16
            z.writestr(zi, entries[n])
        for n, d in extra.items():
            zi = zipfile.ZipInfo(n, date_time=(2019, 1, 1, 0, 0, 0))
            zi.external_attr = 0o644 << 16
            z.writestr(zi, d)
    print("wrote", out_path, os.path.getsize(out_path), "bytes")

if __name__ == "__main__":
    # test package: harmless payload, correct structure
    ub = os.environ.get("UB_FILE")
    if ub:
        ub_bytes = open(ub, "rb").read()
    else:
        ub_bytes = b'#!/system/bin/sh\necho "ui_print testkey package accepted" >&$2\necho "done" >&$2\n'
    ent = {
        "META-INF/com/android/metadata": (
            b"pre-device=p211\r\n"
            b"pre-build-incremental=20231226\r\n"
            b"post-build-incremental=20240101\r\n"
            b"post-timestamp=1704067200\r\n"
        ),
        "META-INF/com/google/android/updater-script": b'assert true;\n',
        "META-INF/com/google/android/update-binary": ub_bytes,
    }
    build(sys.argv[1] if len(sys.argv) > 1 else "~/tvbox/ssrf/pkg/update.zip", ent)
