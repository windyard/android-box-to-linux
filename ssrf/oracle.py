import http.server, threading, time, sys, json

TARGET = [None]
LOG = []

def log(msg):
    line = f'{time.strftime("%H:%M:%S")} {msg}'
    LOG.append(line)
    print(line, flush=True)

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_GET(self):
        log(f'GET {self.path} from {self.client_address[0]} UA={self.headers.get("User-Agent")!r} Range={self.headers.get("Range")!r}')
        if self.path.startswith('/real.wav'):
            data = open('real.wav', 'rb').read()
            self.send_response(200)
            self.send_header('Content-Type', 'audio/x-wav')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('TransferMode.dlna.org', 'Streaming')
            self.end_headers()
            self.wfile.write(data)
        elif self.path.startswith('/r/'):
            TARGET[0] = self.path[3:]
            self.send_response(302)
            self.send_header('Location', TARGET[0])
            self.send_header('Content-Length', '0')
            self.end_headers()
        elif self.path.startswith('/final'):
            body = b'RIFF\x00\x00\x00\x00WAVEfmt ' + b'\x00' * 64
            self.send_response(200)
            self.send_header('Content-Type', 'audio/mpeg')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path.startswith('/slow'):
            time.sleep(10)
            body = b'x' * 10
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            body = b'probe'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def log_message(self, *a):
        pass

http.server.ThreadingHTTPServer(('0.0.0.0', 8888), H).serve_forever()
