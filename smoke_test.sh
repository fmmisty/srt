#!/usr/bin/env bash
# ループバック・スモークテスト: 各コーデックで PCM→SRT→デコード→PCM を往復させ、
# 受信PCMのRMSと推定周波数(L=440Hz / R=660Hz)を確認する。
set -u
cd "$(dirname "$0")"
PORT=9100
SECS=3

run_one() {
    local codec="$1" extra="$2" port="$3"
    local out="/tmp/out_${codec}.raw"
    rm -f "$out"
    # 受信側を先に起動(送信側listen待ちを1秒間隔でリトライ)
    ./srt_recv 127.0.0.1 "$port" --latency=200 > "$out" 2>/tmp/recv_${codec}.log &
    local rpid=$!
    sleep 0.3
    # 送信側: 実時間ペースのトーンを流す
    ./toneutil gen "$SECS" --paced | ./srt_send "$port" --codec="$codec" $extra --latency=200 2>/tmp/send_${codec}.log
    sleep 0.6
    kill "$rpid" 2>/dev/null
    wait "$rpid" 2>/dev/null
    printf '  %-7s ' "$codec"
    ./toneutil check < "$out" 2>&1 | sed 's/^check: //'
}

echo "=== ループバック・スモークテスト (各${SECS}秒) ==="
run_one aac    "--bitrate=128000" $((PORT+0))
run_one aptx   ""                 $((PORT+1))
run_one aptxhd ""                 $((PORT+2))
run_one opus   "--bitrate=128000" $((PORT+3))
run_one lpcm   ""                 $((PORT+4))
echo "=== 完了 (期待値: L~440Hz, R~660Hz, RMS>1000) ==="
