#!/usr/bin/env python3
# 携帯用 コーデック切替WEB ― srt_send の --codec-file / --status-file と連携
#   使い方: python3 codec_web.py [port=8080] [sel=/tmp/codec.sel] [status=/tmp/status.json]
#   携帯ブラウザで http://<PiのIP>:8080/ を開く。ボタンでコーデック選択、現在値を表示。
import sys, os, json, http.server, urllib.parse

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
SEL  = sys.argv[2] if len(sys.argv) > 2 else "/tmp/codec.sel"
STAT = sys.argv[3] if len(sys.argv) > 3 else "/tmp/status.json"

CODECS = [("lpcm","LPCM (無圧縮)"),("aptxhd","aptX HD"),("aptx","aptX"),
          ("aac","AAC"),("opus","Opus")]
VALID = {c for c,_ in CODECS}

PAGE = """<!doctype html><html lang=ja><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>SRT コーデック切替</title>
<style>
 body{font-family:sans-serif;margin:0;background:#111;color:#eee;text-align:center}
 h1{font-size:1.2rem;padding:14px;margin:0;background:#1f4e79}
 .cur{font-size:1.1rem;margin:16px 0;color:#9fd}
 .cur b{font-size:1.5rem;color:#fff}
 .btns{display:flex;flex-direction:column;gap:12px;padding:16px;max-width:420px;margin:0 auto}
 button{font-size:1.4rem;padding:20px;border:0;border-radius:14px;background:#333;color:#eee}
 button.on{background:#2e8b57;color:#fff;font-weight:bold;box-shadow:0 0 0 3px #9f9 inset}
 .note{font-size:.8rem;color:#888;padding:8px}
</style></head><body>
<h1>SRT 音声コーデック切替</h1>
<div class=cur>現在: <b id=cur>―</b></div>
<div class=btns id=btns></div>
<div class=note>ボタンを押すと無停止で切り替わります（受信側も自動追従）</div>
<script>
const C=%s;
const bx=document.getElementById('btns');
C.forEach(([id,label])=>{const b=document.createElement('button');b.textContent=label;b.dataset.id=id;
  b.onclick=()=>fetch('/set?codec='+id).then(()=>refresh());bx.appendChild(b);});
function refresh(){fetch('/status').then(r=>r.json()).then(s=>{
  const c=s.codec||'―';document.getElementById('cur').textContent=c;
  document.querySelectorAll('button').forEach(b=>b.classList.toggle('on',b.dataset.id===c));
}).catch(()=>{});}
refresh();setInterval(refresh,1500);
</script></body></html>""" % json.dumps(CODECS)

class H(http.server.BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="text/html; charset=utf-8"):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/" or u.path == "/index.html":
            self._send(200, PAGE)
        elif u.path == "/status":
            cur = ""
            try: cur = json.load(open(STAT)).get("codec","")
            except Exception:
                try: cur = open(SEL).read().strip()
                except Exception: cur = ""
            self._send(200, json.dumps({"codec": cur}), "application/json")
        elif u.path == "/set":
            q = urllib.parse.parse_qs(u.query); c = (q.get("codec",[""])[0]).lower()
            if c in VALID:
                open(SEL,"w").write(c+"\n")
                self._send(200, json.dumps({"ok": True, "codec": c}), "application/json")
            else:
                self._send(400, json.dumps({"ok": False, "error": "invalid codec"}), "application/json")
        else:
            self._send(404, "not found")
    def log_message(self, *a): pass

if __name__ == "__main__":
    print("codec_web: http://0.0.0.0:%d/  sel=%s status=%s" % (PORT, SEL, STAT))
    http.server.HTTPServer(("0.0.0.0", PORT), H).serve_forever()
