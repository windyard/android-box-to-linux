import hashlib, urllib.request, urllib.parse, json, http.cookiejar, re, sys

R = "http://192.0.2.1"
import os
PASSWORD = os.environ.get("ZTE_ROUTER_PASSWORD", "")  # do not commit the real value

cj = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))

def req(url, data=None):
    h = {'Referer': R + '/', 'Content-Type': 'application/x-www-form-urlencoded'}
    r = urllib.request.Request(R + url, data=data and urllib.parse.urlencode(data).encode(), headers=h)
    return op.open(r, timeout=8).read().decode('utf-8', 'replace')

def login():
    tok = json.loads(req("/?_type=loginsceneData&_tag=login_token_json"))
    lr = json.loads(req("/?_type=loginData&_tag=login_entry", {
        "Username": "admin",
        "Password": hashlib.sha256((PASSWORD + tok['logintoken']).encode()).hexdigest(),
        "action": "login", "Frm_Logintoken": "", "captchaCode": ""}))
    if lr.get('lockingTime') != 0 and not lr.get('sess_token'):
        print("login issue:", json.dumps(lr, ensure_ascii=False))
        return None
    return lr['sess_token']

def get_tag(tag, typ="hiddenData"):
    sess = open('/tmp/zte_sess_token.txt').read().strip()
    return req(f"/?_type={typ}&_tag={tag}&_sessionTOKEN={sess}")

if __name__ == '__main__':
    sess = login()
    if not sess:
        sys.exit(1)
    open('/tmp/zte_sess_token.txt', 'w').write(sess)
    print("login OK")
    for arg in sys.argv[1:]:
        tag, _, typ = arg.partition(':')
        out = get_tag(tag, typ or "hiddenData")
        print(f"===== {tag} =====")
        print(out[:3000])
