import socket, time, sys

BOX = ('192.0.2.125', 38520)
REDIR = 'http://192.0.2.220:8888'

def http_post(path, body, soapaction=''):
    s = socket.create_connection(BOX, 3); s.settimeout(4)
    b = body.encode()
    req = (f'POST {path} HTTP/1.1\r\nHost: x\r\nContent-Type: text/xml\r\n'
           f'SOAPACTION: "{soapaction}"\r\nConnection: close\r\nContent-Length: {len(b)}\r\n\r\n').encode() + b
    s.sendall(req)
    r = b''
    try:
        while len(r) < 4000:
            c = s.recv(1000)
            if not c: break
            r += c
    except socket.timeout: pass
    s.close()
    return r.decode(errors='replace')

def set_uri(url):
    body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            '<s:Body><u:SetAVTransportURI xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
            f'<InstanceID>0</InstanceID><CurrentURI>{url}</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>'
            '</u:SetAVTransportURI></s:Body></s:Envelope>')
    http_post('/service/AVTransport_control', body,
              'urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI')

def transport_state():
    body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            '<s:Body><u:GetTransportInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
            '<InstanceID>0</InstanceID></u:GetTransportInfo></s:Body></s:Envelope>')
    r = http_post('/service/AVTransport_control', body,
                  'urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo')
    for tag in ('CurrentTransportState', 'CurrentTransportStatus'):
        i = r.find(f'<{tag}>')
        if i >= 0:
            j = r.find('</', i)
            return r[i+len(tag)+2:j]
    return None

def set_target(port):
    s = socket.create_connection(('192.0.2.220', 8888), 3); s.settimeout(3)
    s.sendall(f'GET /r/http://127.0.0.1:{port}/x.mp3 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'.encode())
    s.recv(200); s.close()

def probe(port, wait=8):
    set_target(port)
    set_uri(f'{REDIR}/trigger.mp3')
    t0 = time.time()
    while time.time() - t0 < wait:
        time.sleep(0.4)
        st = transport_state()
        if st and st not in ('PLAYING', 'TRANSITIONING', 'NO_MEDIA_PRESENT'):
            return f'{st} in {time.time()-t0:.1f}s'
    return f'no-error after {wait}s (likely OPEN, connection hangs)'

if __name__ == '__main__':
    ports = [int(p) for p in sys.argv[1:]]
    for p in ports:
        print(f'port {p}: {probe(p)}', flush=True)
