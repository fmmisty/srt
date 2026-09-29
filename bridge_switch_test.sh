#!/bin/bash
# hdip_srt_bridge の WEB設定連動(SRT ON/OFF・latency)試験 ― 1台(ループバック)で2拠点を模擬
# ※自社利用版: ライセンス(オプションキー)は無し。SRTのON/OFFはWEB設定の enable のみで決まる。
#
#   偽HDIP-A(127.0.0.3) → Bridge-A(127.0.0.2) ══ SRT/UDP ══ Bridge-B(127.0.0.4) → 偽HDIP-B(127.0.0.5)
#   各HDIPのWEB(srtsetting.py)は模擬HTTPサーバ(8801/8802)。設定JSONを書き換えて途中で切替える。
#
# 必要: make hdip_srt_bridge toneutil、python3
cd "$(dirname "$0")"
W=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf $W' EXIT
FAIL=0; ok(){ echo "OK  $1"; }; ng(){ echo "NG  $1"; FAIL=1; }

MAC_A=02:00:00:00:00:0A; MAC_B=02:00:00:00:00:0B

# 模擬HDIP WEB: GET /script/srtsetting.py → $W/web_<port>.json
cat > $W/web.py <<'EOF'
import http.server, sys
port=int(sys.argv[1]); d=sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(s):
        b=open('%s/web_%d.json'%(d,port),'rb').read()
        s.send_response(200); s.send_header('Content-Type','application/json'); s.end_headers(); s.wfile.write(b)
    def log_message(s,*a): pass
http.server.HTTPServer(('0.0.0.0',port),H).serve_forever()
EOF
# RTP送信(偽HDIPの音声): 16秒・5.33ms毎・1024B・seq 0〜。
# 試験機(WSL等)で送信側自体が止まることがあるので、遅れたら予定を付け直す(実機HDIPのように一気に送らない)。
# 送信側自身の最大停止(ms)を出力 → ブリッジ起因の途切れと区別する
cat > $W/rtptx.py <<'EOF'
import socket, sys, time, struct
ip,port,sec=sys.argv[1],int(sys.argv[2]),float(sys.argv[3])
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); n=int(sec*48000/256); t0=time.time(); stall=0; last=time.time()
for i in range(n):
    now=time.time(); stall=max(stall,now-last); last=now
    s.sendto(struct.pack('!BBHII',0x80,96,i&0xFFFF,i*256,0x12345678)+bytes(1024),(ip,port))
    d=t0+(i+1)*256/48000-time.time()
    if d>0: time.sleep(d)
    elif d<-0.05: t0-=d
print(int(stall*1000))
EOF
# 受信記録: bind ip:port で N秒受け「受信数 最大途切れms 欠落seq数 重複seq数」。第4引数があれば最後の本文を保存
cat > $W/rx.py <<'EOF'
import socket, sys, time, struct
ip,port,sec=sys.argv[1],int(sys.argv[2]),float(sys.argv[3])
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind((ip,port)); s.settimeout(0.1)
n=0; last=None; gap=0; t0=time.time(); body=b''; seqs={}
while time.time()-t0<sec:
    try: d=s.recv(4096)
    except socket.timeout: continue
    now=time.time()
    if last is not None: gap=max(gap,now-last)
    last=now; n+=1; body=d
    if len(d)>=12 and d[0]>>6==2: q=struct.unpack('!H',d[2:4])[0]; seqs[q]=seqs.get(q,0)+1
miss=(max(seqs)+1-len(seqs)) if seqs else 0
dup=sum(c-1 for c in seqs.values())
print(n, int(gap*1000), miss, dup)
if len(sys.argv)>4: open(sys.argv[4],'wb').write(body)
EOF
web(){ # web <port> <mac> <enable> <latency>   ※keyフィールドは互換のため残すが常に空(ブリッジは無視)
  printf '{"mac": "%s", "ifs": {}, "srt": {"enable": "%s", "latency_ms": "%s", "key": ""}}' "$2" "$3" "$4" > $W/web_$1.json.tmp
  mv $W/web_$1.json.tmp $W/web_$1.json; }

web 8801 $MAC_A 0 250; web 8802 $MAC_B 0 250
python3 $W/web.py 8801 $W & python3 $W/web.py 8802 $W &
sleep 0.5
./hdip_srt_bridge --peer=127.0.0.1 --tx-port=9000 --rx-port=9001 --udp-port=9100 --peer-udp-port=9101 \
    --local-ip=127.0.0.2 --dev-ip=127.0.0.3 --web-port=8801 --poll=1 2> $W/A.log &
./hdip_srt_bridge --peer=127.0.0.1 --tx-port=9001 --rx-port=9000 --udp-port=9101 --peer-udp-port=9100 \
    --local-ip=127.0.0.4 --dev-ip=127.0.0.5 --web-port=8802 --poll=1 2> $W/B.log &
sleep 2

echo "--- 1) 音声A→B 16秒: OFF(UDP)→4s:enable=1でSRT ON(120ms)→8s:latency 250→12s:OFF(UDP)"
python3 $W/rx.py 127.0.0.5 15000 20 > $W/rx1.txt &
RX=$!
python3 $W/rtptx.py 127.0.0.2 15000 16 > $W/tx1.txt &
sleep 4;  web 8801 $MAC_A 1 120
sleep 4;  web 8801 $MAC_A 1 250
sleep 4;  web 8801 $MAC_A 0 250
wait $RX
read N GAP MISS DUP < $W/rx1.txt
read TXSTALL < $W/tx1.txt
SENT=$((16*48000/256))   # 16秒=3000パケット
echo "    受信 $N / 送信 $SENT パケット, 欠落 $MISS, 重複 $DUP, 最大途切れ ${GAP}ms (送信側自身の最大停止 ${TXSTALL}ms)"
[ "$N" = "$SENT" ] && [ "$MISS" = 0 ] && ok "切替を跨いで欠落ゼロ(3000/3000)" || ng "欠落あり"
[ "$DUP" = 0 ] && ok "重複ゼロ(UDP/SRT二重送信は受け側で除去)" || ng "重複 $DUP"
# 途切れ=SRT切替で増えるlatency分(最大250ms)まで許容。送信側自身が止まった分は除く
[ "$GAP" -lt 300 ] || [ "$GAP" -le $((TXSTALL+100)) ] && ok "ブリッジ起因の途切れ 0.3秒未満" || ng "途切れ ${GAP}ms"
grep -q "設定変更: SRT ON  latency=120ms" $W/A.log && ok "A: enable=1でSRT ON" || ng "A: SRT ONにならない"
grep -q "送り=SRT: .*latency=120ms" $W/A.log && ok "A: 送りがSRT(120ms)に切替" || ng "A: SRT送りなし"
grep -q "受けSRT: 127.0.0.1:9000 へ接続" $W/B.log && ok "B: SRTで受信" || ng "B: SRT受信なし"
grep -q "送り=SRT: .*latency=250ms" $W/A.log && ok "A: latency 250msで張り直し" || ng "A: latency変更が反映されない"
grep -q "設定変更: WEB設定でSRT OFF" $W/A.log && ok "A: OFFでUDPに戻る" || ng "A: OFFにならない"

echo "--- 2) 呼制御(ALG): SRC-ADDR/MEDIA-ADDRがB側Bridge(127.0.0.4)に書き換わる"
for MODE in UDP SRT; do
  [ $MODE = SRT ] && { web 8801 $MAC_A 1 120; sleep 3; }
  python3 $W/rx.py 127.0.0.5 23500 2 $W/ctl.txt > /dev/null & CR=$!
  sleep 0.3
  printf 'VERSION:1\r\nCOMMAND:CALL\r\nSRC-ADDR:127.0.0.3\r\nMEDIA-ADDR:127.0.0.3\r\nA-PORT:15000\r\n' | ./toneutil udptext 127.0.0.2 23500
  wait $CR
  grep -q "SRC-ADDR:127.0.0.4" $W/ctl.txt && grep -q "MEDIA-ADDR:127.0.0.4" $W/ctl.txt && grep -q "A-PORT:15000" $W/ctl.txt \
    && ok "ALG書換($MODE)" || ng "ALG書換($MODE)"
done

echo "--- 3) HDIPのWEBが応答しない時は直前の設定を維持"
web 8801 $MAC_A 1 120; sleep 2.5
kill %1 2>/dev/null; sleep 2.5
grep -q "取得できません.*直前の設定を維持" $W/A.log && ok "WEB無応答でも設定維持" || ng "WEB無応答時の扱い"
tail -1 $W/A.log | grep -q "設定変更" && ng "WEB無応答で設定が変わった" || ok "WEB無応答で切替は起きない"

if [ $FAIL = 0 ]; then echo "全テスト合格"; else echo "失敗あり"; echo "== A.log"; cat $W/A.log; echo "== B.log"; cat $W/B.log; exit 1; fi
