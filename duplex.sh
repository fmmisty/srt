#!/usr/bin/env bash
#
# duplex.sh  ―  SRT音声 双方向(フル二重)起動スクリプト
#
# 各拠点で send(自分がlisten) と recv(相手へconnect) を1本ずつ立ち上げる。
# 両拠点で同じスクリプトを、tx-port と rx-port を入れ替えて実行する。
#
#   A地点:  ./duplex.sh --peer=<B_IP> --tx-port=9000 --rx-port=9001
#   B地点:  ./duplex.sh --peer=<A_IP> --tx-port=9001 --rx-port=9000
#
# 主なオプション(=の後に値):
#   --peer       相手のIPアドレス           (必須)
#   --tx-port    自分の送信listenポート      (既定 9000)  ※相手の --rx-port と一致させる
#   --rx-port    相手の送信listenポート      (既定 9001)  ※相手の --tx-port と一致させる
#   --codec      aac|aptx|aptxhd|opus|lpcm  (既定 aac)
#   --bitrate    AAC/Opusのビットレート      (既定 128000)
#   --latency    SRTバッファ(ms)             (既定 250)
#   --dev        ALSAデバイス               (既定 plughw:1,0)  ※arecord -l で確認
#   --rate       サンプルレート             (既定 48000)
#   --ch         チャンネル数               (既定 2)
#   --passphrase 暗号化パスフレーズ(10〜79字) (任意。両拠点で同一に)
#
set -u
cd "$(dirname "$0")"

PEER="" ; TX_PORT=9000 ; RX_PORT=9001
CODEC=aac ; BITRATE=128000 ; LATENCY=250
DEV=plughw:1,0 ; RATE=48000 ; CH=2 ; PASS=""

for a in "$@"; do
  case "$a" in
    --peer=*)       PEER="${a#*=}" ;;
    --tx-port=*)    TX_PORT="${a#*=}" ;;
    --rx-port=*)    RX_PORT="${a#*=}" ;;
    --codec=*)      CODEC="${a#*=}" ;;
    --bitrate=*)    BITRATE="${a#*=}" ;;
    --latency=*)    LATENCY="${a#*=}" ;;
    --dev=*)        DEV="${a#*=}" ;;
    --rate=*)       RATE="${a#*=}" ;;
    --ch=*)         CH="${a#*=}" ;;
    --passphrase=*) PASS="${a#*=}" ;;
    -h|--help)      sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "不明なオプション: $a" >&2; exit 1 ;;
  esac
done

if [ -z "$PEER" ]; then echo "エラー: --peer=<相手IP> は必須です。--help 参照。" >&2; exit 1; fi
if [ ! -x ./srt_send ] || [ ! -x ./srt_recv ]; then echo "エラー: srt_send/srt_recv が無い。先に make。" >&2; exit 1; fi

PASS_OPT=""
[ -n "$PASS" ] && PASS_OPT="--passphrase=$PASS"

echo "=== 双方向STL起動 ==="
echo "  相手       : $PEER"
echo "  送信(TX)   : 自分がlisten :$TX_PORT  codec=$CODEC latency=${LATENCY}ms"
echo "  受信(RX)   : $PEER:$RX_PORT へ接続"
echo "  音声デバイス: $DEV  ${RATE}Hz ${CH}ch"
[ -n "$PASS" ] && echo "  暗号化     : 有効"
echo "  停止       : Ctrl+C"
echo "===================="

PIDS=()
CLEANED=0
cleanup() {
  [ "$CLEANED" = 1 ] && return; CLEANED=1
  echo ""; echo "[duplex] 停止中..."
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  wait 2>/dev/null
  echo "[duplex] 終了。"
}
trap cleanup INT TERM

# 送信: マイク → arecord → srt_send(listen)
( arecord -D "$DEV" -f S16_LE -r "$RATE" -c "$CH" -t raw 2>/dev/null \
    | ./srt_send "$TX_PORT" --codec="$CODEC" --bitrate="$BITRATE" \
                 --samplerate="$RATE" --channels="$CH" --latency="$LATENCY" $PASS_OPT ) &
PIDS+=($!)

# 受信: srt_recv(connect) → aplay → スピーカ
( ./srt_recv "$PEER" "$RX_PORT" --latency="$LATENCY" $PASS_OPT \
    | aplay -D "$DEV" -f S16_LE -r "$RATE" -c "$CH" -t raw 2>/dev/null ) &
PIDS+=($!)

wait
