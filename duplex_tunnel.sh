#!/usr/bin/env bash
#
# duplex_tunnel.sh  ―  RTPの頭にSRTを被せる透過トンネルを双方向で起動(自作版)
#
# 装置(HDIP-3000V等)のUDP/RTPを中身を触らずSRTで運ぶ udp2srt(送り)/srt2udp(受け)を
# 各拠点で1本ずつ立ち上げる。コーデックは装置側で選択(CLEAR/Opus/SBADPCM/LPCM等)。
# srt-live-transmit 不要(自作バイナリで完結)。両拠点で tx/rx を入れ替えて実行。
#
#   A拠点: ./duplex_tunnel.sh --peer=<B_IP> --tx-port=9000 --rx-port=9001 \
#            --dev-port=15000 --dev-ip=<A装置IP>
#   B拠点: ./duplex_tunnel.sh --peer=<A_IP> --tx-port=9001 --rx-port=9000 \
#            --dev-port=15000 --dev-ip=<B装置IP>
#
# オプション:
#   --peer       相手拠点のIP(SRT接続先)          (必須)
#   --tx-port    自分のSRT待受ポート               (既定 9000) ※相手の --rx-port と一致
#   --rx-port    相手のSRT待受ポート               (既定 9001) ※相手の --tx-port と一致
#   --dev-port   装置のRTP送受ポート                (既定 15000: 装置が送出=ここで受け、装置が受信=ここへ配る)
#   --dev-ip     自拠点の装置IP(受信を配る先)        (既定 127.0.0.1)
#   --latency    SRTバッファ(ms)                    (既定 250)
#   --passphrase 暗号化(10〜79字, 両拠点同一)        (任意)
#
# 装置側設定: IP接続モード / RTP送信先= このPi:dev-port / RTP受信ポート= dev-port
#
set -u
cd "$(dirname "$0")"

PEER="" ; TX_PORT=9000 ; RX_PORT=9001
DEV_PORT=15000 ; DEV_IP="127.0.0.1" ; LATENCY=250 ; PASS=""

for a in "$@"; do
  case "$a" in
    --peer=*)       PEER="${a#*=}" ;;
    --tx-port=*)    TX_PORT="${a#*=}" ;;
    --rx-port=*)    RX_PORT="${a#*=}" ;;
    --dev-port=*)   DEV_PORT="${a#*=}" ;;
    --dev-ip=*)     DEV_IP="${a#*=}" ;;
    --latency=*)    LATENCY="${a#*=}" ;;
    --passphrase=*) PASS="${a#*=}" ;;
    -h|--help)      sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "不明なオプション: $a" >&2; exit 1 ;;
  esac
done

if [ -z "$PEER" ]; then echo "エラー: --peer=<相手IP> は必須です。--help 参照。" >&2; exit 1; fi
if [ ! -x ./udp2srt ] || [ ! -x ./srt2udp ]; then echo "エラー: udp2srt/srt2udp が無い。先に make。" >&2; exit 1; fi

PASS_OPT=""; [ -n "$PASS" ] && PASS_OPT="--passphrase=$PASS"

echo "=== 双方向 透過トンネル(RTP透過/SRT) 起動 ==="
echo "  相手            : $PEER"
echo "  送り: 装置UDP :$DEV_PORT → SRT listen :$TX_PORT"
echo "  受け: SRT $PEER:$RX_PORT → 装置UDP $DEV_IP:$DEV_PORT"
echo "  latency=${LATENCY}ms$([ -n "$PASS" ] && echo '  暗号化:有効')"
echo "  停止: Ctrl+C"
echo "==========================================="

PIDS=() ; CLEANED=0
cleanup() {
  [ "$CLEANED" = 1 ] && return; CLEANED=1
  echo ""; echo "[tunnel] 停止中..."
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  wait 2>/dev/null
  echo "[tunnel] 終了。"
}
trap cleanup INT TERM

# 送り: 装置UDP(:DEV_PORT) → SRT(listen :TX_PORT)
./udp2srt "$TX_PORT" --udp-in="$DEV_PORT" --latency="$LATENCY" $PASS_OPT &
PIDS+=($!)

# 受け: SRT(connect PEER:RX_PORT) → 装置UDP(DEV_IP:DEV_PORT)
./srt2udp "$PEER" "$RX_PORT" --udp-out="$DEV_IP:$DEV_PORT" --latency="$LATENCY" $PASS_OPT &
PIDS+=($!)

wait
