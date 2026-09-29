#!/usr/bin/env bash
# 透過トンネル(udp2srt/srt2udp)のロス/遅延/ジッタ耐性試験。
# 経路: toneutil(装置送出) → udp2srt → [udp_proxy 劣化] → srt2udp → toneutil(装置受け)
set -u
cd "$(dirname "$0")"
SECS=6

RUN=0
run() {
    local name="$1" loss="$2" delay="$3" jit="$4" lat="$5"
    RUN=$((RUN+1)); local base=$((10000 + RUN*10))
    local IN=$((base+1)) SRT=$((base+2)) PROXY=$((base+3)) OUT=$((base+4))
    local OUTF="/tmp/tl_out_${RUN}.raw"; rm -f "$OUTF"
    timeout $((SECS+6)) ./toneutil udprecv $OUT > "$OUTF" 2>/dev/null &
    timeout $((SECS+6)) ./udp2srt $SRT --udp-in=$IN --latency=$lat 2>/dev/null &
    sleep 0.2
    timeout $((SECS+6)) ./udp_proxy --listen=$PROXY --forward=127.0.0.1:$SRT --loss=$loss --delay=$delay --jitter=$jit 2>/dev/null &
    sleep 0.2
    timeout $((SECS+6)) ./srt2udp 127.0.0.1 $PROXY --udp-out=127.0.0.1:$OUT --latency=$lat 2>/dev/null &
    sleep 0.4
    timeout $((SECS+2)) ./toneutil udpsend 127.0.0.1 $IN $SECS
    sleep 2.5
    printf '  %-28s ' "$name"
    ./toneutil check < "$OUTF" 2>&1 | sed 's/^check: //'
}

echo "=== 透過トンネル 劣化耐性試験 (各${SECS}s, 装置=1.536Mbps相当) ==="
run "クリーン"                       0  0   0  200
run "ロス5% 遅延40 ジッタ15"          5  40  15 300
run "ロス10% 遅延80 ジッタ30"         10 80  30 400
run "ロス20% 遅延100 ジッタ40"        20 100 40 500
echo "=== 期待: dur≈${SECS}s維持, L~440/R~660Hz ==="
