import http.server

TARGET = ['http://127.0.0.1:9/']

class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith('/r/'):
            TARGET[0] = self.path[3:]
        self.send_response(302)
        self.send_header('Location', TARGET[0])
        self.end_headers()

    def log_message(self, *a):
        pass

http.server.HTTPServer(('0.0.0.0', 8888), H).serve_forever()
