#!/bin/bash
# 無停止コーデック切替テスト: 送信中に codec-file を aac→opus→lpcm と書き換え、
# 受信側が追従し音(440/660Hz)が続くか確認する。
cd "$(dirname "$0")"
pkill -x srt_send; pkill -x srt_recv; pkill -x toneutil; sleep 0.5
SEL=/tmp/codec.sel; ST=/tmp/status.json
echo aac > $SEL
timeout 16 ./srt_recv 127.0.0.1 9500 --latency=200 > /tmp/rt_out.raw 2>/tmp/rt_recv.log &
sleep 0.3
( ./toneutil gen 12 --paced | timeout 16 ./srt_send 9500 --codec-file=$SEL --status-file=$ST --latency=200 2>/tmp/rt_send.log ) &
sleep 4;  echo opus > $SEL
sleep 4;  echo lpcm > $SEL
sleep 5
pkill -x srt_send; pkill -x srt_recv; pkill -x toneutil; sleep 0.5
echo "=== status.json (現在コーデック) ==="; cat $ST; echo
echo "=== 送信側 切替ログ ==="; grep -E "コーデック切替|codec=" /tmp/rt_send.log
echo "=== 受信側 切替ログ ==="; grep -E "デコーダ" /tmp/rt_recv.log
echo "=== 受信PCM 全体のトーン(440/660が保たれていれば切替後も鳴っている) ==="; ./toneutil check < /tmp/rt_out.raw 2>&1
