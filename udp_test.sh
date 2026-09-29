#!/usr/bin/env bash
# UDP→SRT→UDP エンドツーエンド検証。
# 装置(UDP PCM) → srt_send --udp-in → SRT(圧縮) → srt_recv --udp-out → 受信(UDP PCM)
set -u
cd "$(dirname "$0")"
SECS=3

run_one() {
    local codec="$1" extra="$2" sp="$3"
    local inport=$((sp+1000)) outport=$((sp+2000))
    local out="/tmp/udp_${codec}.raw"
    rm -f "$out"
    # 最終UDP出力を捕捉(装置=受信機器の代わり)
    ./toneutil udprecv "$outport" > "$out" 2>/dev/null &
    local rpid=$!
    sleep 0.2
    # 受信ブリッジ: SRT → UDP(:outport)
    ./srt_recv 127.0.0.1 "$sp" --latency=200 --udp-out=127.0.0.1:"$outport" >/dev/null 2>/tmp/r_${codec}.log &
    local srpid=$!
    sleep 0.2
    # 送信ブリッジ: UDP(:inport) → SRT(listen :sp)
    ./srt_send "$sp" --codec="$codec" $extra --latency=200 --udp-in="$inport" >/dev/null 2>/tmp/s_${codec}.log &
    local sspid=$!
    sleep 0.3
    # 装置シミュレータ: UDPでPCMを送る(実時間)
    ./toneutil udpsend 127.0.0.1 "$inport" "$SECS"
    sleep 0.8
    kill "$sspid" "$srpid" 2>/dev/null
    wait "$rpid" 2>/dev/null
    printf '  %-7s ' "$codec"
    ./toneutil check < "$out" 2>&1 | sed 's/^check: //'
}

echo "=== UDP↔SRT↔UDP エンドツーエンド (各${SECS}秒) ==="
run_one lpcm   ""                 9300
run_one aptxhd ""                 9310
run_one opus   "--bitrate=128000" 9320
run_one aac    "--bitrate=128000" 9330
echo "=== 完了 (期待: L~440Hz R~660Hz RMS>1000) ==="
