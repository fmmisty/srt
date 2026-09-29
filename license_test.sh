#!/bin/bash
# SRTオプションキーの発行/検証テスト(社内用)
cd "$(dirname "$0")"
PRIV=../srt_license_private/srt_license_private.pem
ok(){ echo "OK  $1"; }; ng(){ echo "NG  $1"; FAIL=1; }
K=$(./srt_keygen --priv=$PRIV --mac=00:1A:2B:3C:4D:5E --features=ip 2>/dev/null)
./srt_keygen --verify="$K" --mac=00:1a:2b:3c:4d:5e | grep -q "有効: 機能=SRT(IP接続)" && ok "正しいMACで有効(ip)" || ng "正しいMAC"
./srt_keygen --verify="$K" --mac=00:1A:2B:3C:4D:5F >/dev/null && ng "別MACで有効になった" || ok "別MACでは無効"
KL=$(echo "$K" | tr A-Z a-z | tr -d -)
./srt_keygen --verify="$KL" --mac=001A2B3C4D5E >/dev/null && ok "小文字・ハイフン無しでも有効" || ng "小文字"
BAD="${K:0:10}$( [ "${K:10:1}" = A ] && echo B || echo A )${K:11}"
./srt_keygen --verify="$BAD" --mac=001A2B3C4D5E >/dev/null && ng "改ざんキーが有効" || ok "1文字改ざんで無効"
# 機能ビットを書き換えて ip→all に昇格させる偽造
FORGE="$(echo "$K" | sed 's/^AE/AI/')"
./srt_keygen --verify="$FORGE" --mac=001A2B3C4D5E >/dev/null && ng "機能昇格の偽造が有効" || ok "機能ビット書換(昇格)で無効"
KA=$(./srt_keygen --priv=$PRIV --mac=001A2B3C4D5E --features=all 2>/dev/null)
./srt_keygen --verify="$KA" --mac=001A2B3C4D5E | grep -q "SRT(IP接続) SRT(NGN/CLEAR)" && ok "all=両機能" || ng "all"
./srt_keygen --verify="" --mac=001A2B3C4D5E >/dev/null && ng "空キー有効" || ok "空キーは無効"
[ -z "$FAIL" ] && echo "全テスト合格" || { echo "失敗あり"; exit 1; }
