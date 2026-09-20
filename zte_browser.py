import cdp, hashlib, json, time, sys

URL = "http://192.0.2.1/"
import os
PW = os.environ.get("ZTE_ROUTER_PASSWORD", "")  # do not commit the real value
TAB_KEY = "zte_router_tab"

def find_tab():
    for t in cdp.http_json("/json"):
        if t.get("type") == "page" and t.get("url", "").startswith("http://192.0.2.1"):
            return cdp.Tab(t)
    return None

def session():
    tab = find_tab()
    if tab is None:
        tab = cdp.Tab(cdp.new_tab(URL))
        tab.navigate(URL)
        time.sleep(2)
    tok = json.loads(tab.eval("fetch('/?_type=loginsceneData&_tag=login_token_json').then(r=>r.text())"))
    pw = hashlib.sha256((PW + tok['logintoken']).encode()).hexdigest()
    body = f"Username=admin&Password={pw}&action=login&Frm_Logintoken=&captchaCode=&_sessionTOKEN={tok['_sessionToken']}"
    r = json.loads(tab.eval(f"fetch('/?_type=loginData&_tag=login_entry',{{method:'POST',headers:{{'Content-Type':'text/xml;charset=utf-8'}},body:{json.dumps(body)}}}).then(r=>r.text())"))
    return tab, r['sess_token']

def get_tag(tab, sess, tag, typ="hiddenData"):
    return tab.eval(f"fetch('/?_type={typ}&_tag={tag}&_sessionTOKEN={sess}').then(r=>r.text())")

if __name__ == '__main__':
    tab, sess = session()
    for arg in sys.argv[1:]:
        tag, _, typ = arg.partition(':')
        out = get_tag(tab, sess, tag, typ or "hiddenData")
        fn = f"/tmp/zte_{tag}.xml"
        open(fn, "w").write(out or "NONE")
        print(f"===== {tag} -> {fn} ({len(out or '')} bytes)")
        print((out or "")[:800])
