#!/usr/bin/env bash
#
# duplex_bridge.sh  ―  既存UDP装置を無改造でSRT化する「双方向ブリッジ」
#
# 圧縮済みUDPストリームをそのままSRTに載せ替える(デコード/エンコードなし)。
# srt-live-transmit を各拠点で2本(送り: udp→srt / 受け: srt→udp)立ち上げる。
# 装置側は一切改造せず、UDPの送信先をこのブリッジに向けるだけ。
#
# 必要: srt-live-transmit  (Debian/RaspberryPiOS: sudo apt install srt-tools)
#
#   A拠点: ./duplex_bridge.sh --peer=<B_IP> \
#            --tx-port=9000 --rx-port=9001 \
#            --in-port=1234 --out-dest=127.0.0.1:5678
#   B拠点: ./duplex_bridge.sh --peer=<A_IP> \
#            --tx-port=9001 --rx-port=9000 \
#            --in-port=1234 --out-dest=127.0.0.1:5678
#
# オプション(=の後に値):
#   --peer       相手のIP                         (必須)
#   --tx-port    自分のSRT送信listenポート          (既定 9000) ※相手の --rx-port と一致
#   --rx-port    相手のSRT送信listenポート          (既定 9001) ※相手の --tx-port と一致
#   --in-port    装置がこのPiへUDP送信してくるポート  (既定 1234) → SRTへ載せる
#   --out-dest   受信SRTをUDPで配る宛先 host:port    (既定 127.0.0.1:5678) → 受信機器へ
#   --latency    SRTバッファ(ms)                    (既定 250)
#   --passphrase 暗号化パスフレーズ(10〜79字)        (任意。両拠点で同一に)
#   --bind       装置UDPを受ける待受アドレス          (既定 0.0.0.0)
#
set -u

PEER="" ; TX_PORT=9000 ; RX_PORT=9001
IN_PORT=1234 ; OUT_DEST="127.0.0.1:5678"
LATENCY=250 ; PASS="" ; BIND="0.0.0.0"

for a in "$@"; do
  case "$a" in
    --peer=*)       PEER="${a#*=}" ;;
    --tx-port=*)    TX_PORT="${a#*=}" ;;
    --rx-port=*)    RX_PORT="${a#*=}" ;;
    --in-port=*)    IN_PORT="${a#*=}" ;;
    --out-dest=*)   OUT_DEST="${a#*=}" ;;
    --latency=*)    LATENCY="${a#*=}" ;;
    --passphrase=*) PASS="${a#*=}" ;;
    --bind=*)       BIND="${a#*=}" ;;
    -h|--help)      sed -n '2,32p' "$0"; exit 0 ;;
    *) echo "不明なオプション: $a" >&2; exit 1 ;;
  esac
done

if [ -z "$PEER" ]; then echo "エラー: --peer=<相手IP> は必須です。--help 参照。" >&2; exit 1; fi
if ! command -v srt-live-transmit >/dev/null 2>&1; then
  echo "エラー: srt-live-transmit が無い。 sudo apt install srt-tools" >&2; exit 1
fi

# SRT URLの共通パラメータ(latency/暗号化)
Q="latency=$LATENCY"
[ -n "$PASS" ] && Q="$Q&passphrase=$PASS"

echo "=== 双方向UDP↔SRTブリッジ 起動 ==="
echo "  相手              : $PEER"
echo "  送り(TX) 装置UDP :$IN_PORT  → SRT listen :$TX_PORT"
echo "  受け(RX) SRT $PEER:$RX_PORT → 装置UDP $OUT_DEST"
echo "  latency=${LATENCY}ms$([ -n "$PASS" ] && echo '  暗号化:有効')"
echo "  停止: Ctrl+C"
echo "================================"

PIDS=() ; CLEANED=0
cleanup() {
  [ "$CLEANED" = 1 ] && return; CLEANED=1
  echo ""; echo "[bridge] 停止中..."
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  wait 2>/dev/null
  echo "[bridge] 終了。"
}
trap cleanup INT TERM

# 送り: 装置UDP(:IN_PORT) → SRT(listen :TX_PORT)
srt-live-transmit "udp://${BIND}:${IN_PORT}" "srt://:${TX_PORT}?${Q}" -v &
PIDS+=($!)

# 受け: SRT(connect PEER:RX_PORT) → 装置UDP(OUT_DEST)
srt-live-transmit "srt://${PEER}:${RX_PORT}?${Q}" "udp://${OUT_DEST}" -v &
PIDS+=($!)

wait
