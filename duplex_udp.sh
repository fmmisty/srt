#!/usr/bin/env bash
#
# duplex_udp.sh  ―  PCMをUDPで受け、SRT側で圧縮して双方向伝送する起動スクリプト
#
# 装置がPCMをUDPで出す → srt_send --udp-in で圧縮(コーデック選択) → SRT → 相手 →
# srt_recv --udp-out でデコード → 受信機器へUDPでPCMを配る。装置は無改造。
# 各拠点で srt_send(自分がlisten) と srt_recv(相手へconnect) を1本ずつ立ち上げる。
# 両拠点で同じスクリプトを、tx/rx ポートを入れ替えて実行する。
#
#   A拠点: ./duplex_udp.sh --peer=<B_IP> --tx-port=9000 --rx-port=9001 \
#            --in-port=1234 --out-dest=192.168.0.20:5678 --codec=aptxhd
#   B拠点: ./duplex_udp.sh --peer=<A_IP> --tx-port=9001 --rx-port=9000 \
#            --in-port=1234 --out-dest=192.168.0.30:5678 --codec=aptxhd
#
# オプション(=の後に値):
#   --peer       相手のIP                        (必須)
#   --tx-port    自分のSRT送信listenポート         (既定 9000) ※相手の --rx-port と一致
#   --rx-port    相手のSRT送信listenポート         (既定 9001) ※相手の --tx-port と一致
#   --in-port    装置がこのPiへPCMをUDP送信するポート (既定 1234)
#   --out-dest   受信PCMを配るUDP宛先 host:port     (既定 127.0.0.1:5678)
#   --codec      lpcm|aptx|aptxhd|opus|aac         (既定 aptxhd ※1M回線でも収まる高音質)
#   --bitrate    AAC/Opusのビットレート            (既定 128000)
#   --samplerate サンプルレート                    (既定 48000)
#   --channels   チャンネル数                      (既定 2)
#   --latency    SRTバッファ(ms)                   (既定 250)
#   --passphrase 暗号化パスフレーズ(10〜79字)       (任意。両拠点で同一に)
#
set -u
cd "$(dirname "$0")"

PEER="" ; TX_PORT=9000 ; RX_PORT=9001
IN_PORT=1234 ; OUT_DEST="127.0.0.1:5678"
CODEC=aptxhd ; BITRATE=128000 ; RATE=48000 ; CH=2
LATENCY=250 ; PASS=""

for a in "$@"; do
  case "$a" in
    --peer=*)       PEER="${a#*=}" ;;
    --tx-port=*)    TX_PORT="${a#*=}" ;;
    --rx-port=*)    RX_PORT="${a#*=}" ;;
    --in-port=*)    IN_PORT="${a#*=}" ;;
    --out-dest=*)   OUT_DEST="${a#*=}" ;;
    --codec=*)      CODEC="${a#*=}" ;;
    --bitrate=*)    BITRATE="${a#*=}" ;;
    --samplerate=*) RATE="${a#*=}" ;;
    --channels=*)   CH="${a#*=}" ;;
    --latency=*)    LATENCY="${a#*=}" ;;
    --passphrase=*) PASS="${a#*=}" ;;
    -h|--help)      sed -n '2,34p' "$0"; exit 0 ;;
    *) echo "不明なオプション: $a" >&2; exit 1 ;;
  esac
done

if [ -z "$PEER" ]; then echo "エラー: --peer=<相手IP> は必須です。--help 参照。" >&2; exit 1; fi
if [ ! -x ./srt_send ] || [ ! -x ./srt_recv ]; then echo "エラー: srt_send/srt_recv が無い。先に make。" >&2; exit 1; fi

PASS_OPT=""
[ -n "$PASS" ] && PASS_OPT="--passphrase=$PASS"

echo "=== 双方向 UDP-PCM ⇔ SRT圧縮 起動 ==="
echo "  相手            : $PEER"
echo "  送り(TX): 装置UDP :$IN_PORT → [$CODEC 圧縮] → SRT listen :$TX_PORT"
echo "  受け(RX): SRT $PEER:$RX_PORT → [デコード] → 装置UDP $OUT_DEST"
echo "  ${RATE}Hz ${CH}ch  latency=${LATENCY}ms$([ -n "$PASS" ] && echo '  暗号化:有効')"
echo "  停止: Ctrl+C"
echo "===================================="

PIDS=() ; CLEANED=0
cleanup() {
  [ "$CLEANED" = 1 ] && return; CLEANED=1
  echo ""; echo "[duplex_udp] 停止中..."
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  wait 2>/dev/null
  echo "[duplex_udp] 終了。"
}
trap cleanup INT TERM

# 送り: 装置UDP(:IN_PORT) → 圧縮 → SRT(listen :TX_PORT)
./srt_send "$TX_PORT" --codec="$CODEC" --bitrate="$BITRATE" \
           --samplerate="$RATE" --channels="$CH" --latency="$LATENCY" \
           --udp-in="$IN_PORT" $PASS_OPT &
PIDS+=($!)

# 受け: SRT(connect PEER:RX_PORT) → デコード → 装置UDP(OUT_DEST)
./srt_recv "$PEER" "$RX_PORT" --latency="$LATENCY" \
           --udp-out="$OUT_DEST" $PASS_OPT &
PIDS+=($!)

wait
