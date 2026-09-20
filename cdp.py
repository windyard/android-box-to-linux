import json, sys, urllib.request

import websocket

BASE = "http://localhost:9876"

def http_json(path, method="GET"):
    req = urllib.request.Request(BASE + path, method=method)
    return json.loads(urllib.request.urlopen(req, timeout=5).read())

def new_tab(url):
    try:
        return http_json("/json/new?" + urllib.parse.quote(url, safe=""), "PUT")
    except Exception:
        targets = http_json("/json")
        return next((t for t in targets if t.get("url") == url), None)

class Tab:
    def __init__(self, target):
        self.ws = websocket.create_connection(target["webSocketDebuggerUrl"], timeout=30, suppress_origin=True)
        self.id = 0

    def cmd(self, method, **params):
        self.id += 1
        self.ws.send(json.dumps({"id": self.id, "method": method, "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == self.id:
                if "error" in msg:
                    raise RuntimeError(msg["error"])
                return msg.get("result", {})

    def navigate(self, url):
        self.cmd("Page.enable")
        self.cmd("Page.navigate", url=url)

    def eval(self, expr, await_promise=True):
        self.cmd("Runtime.enable")
        r = self.cmd("Runtime.evaluate", expression=expr,
                    returnByValue=True, awaitPromise=await_promise)
        res = r.get("result", {})
        if r.get("exceptionDetails"):
            return {"exception": r["exceptionDetails"].get("exception", {}).get("description", str(r["exceptionDetails"]))[:500]}
        return res.get("value")

import urllib.parse
