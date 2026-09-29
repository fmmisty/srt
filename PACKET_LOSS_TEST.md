# パケットロス耐性検証（tc netem）

## 目的
`tc netem` で人工的にパケットロス・遅延・ジッタを付与し、`latency_ms` の適正値と、AAC欠落時の挙動を確認する。
現状の `aac_srt_recv.c` は、AACデコードエラー時に該当フレームをスキップして継続するため、ロスが増えると短い音切れとして現れる。

## 事前確認
```bash
ip link
sudo apt update
sudo apt install -y iproute2
```

## netem設定例
以下は `eth0` の例。実機では `ip link` で確認したIF名に変更する。

```bash
# 1% ロス
sudo tc qdisc add dev eth0 root netem loss 1%

# 100ms遅延 + 20msジッタ
sudo tc qdisc replace dev eth0 root netem delay 100ms 20ms distribution normal

# 100ms遅延 + 20msジッタ + 1%ロス
sudo tc qdisc replace dev eth0 root netem delay 100ms 20ms distribution normal loss 1%

# 確認
tc qdisc show dev eth0

# 解除
sudo tc qdisc del dev eth0 root
```

## 試験パターン
| No. | netem条件 | latency_ms | 確認内容 |
|---:|---|---:|---|
| 1 | 劣化なし | 150 | 基準音質・ログ確認 |
| 2 | loss 0.1% | 150 / 300 | 軽微ロス時の再送効果 |
| 3 | loss 1% | 150 / 300 / 500 | latency差による改善度 |
| 4 | delay 100ms 20ms | 150 / 300 | 遅延・ジッタ吸収 |
| 5 | delay 200ms 50ms loss 1% | 300 / 500 | 厳しめ条件での限界確認 |
| 6 | loss 3% | 300 / 500 | AAC欠落時の破綻確認 |

## 送受信コマンド例
送信側:
```bash
arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -t raw \
  | ./aac_srt_send 9000 48000 2 96000 300
```

受信側:
```bash
./aac_srt_recv <Pi AのIP> 9000 300 \
  | aplay -f S16_LE -r 48000 -c 2 -t raw
```

録音して比較する場合:
```bash
./aac_srt_recv <Pi AのIP> 9000 300 > recv_300ms.raw
aplay -f S16_LE -r 48000 -c 2 -t raw recv_300ms.raw
```

## 判定
- 150msで音切れが出るが300msで改善する場合、無線区間のRTT・ジッタに対して150msが不足。
- 300msでも改善しない場合、ロス率がSRT再送で吸収できる範囲を超えている可能性がある。
- AAC欠落時は現状スキップのみ。改善するなら、無音挿入、前回PCM再利用、フレーム番号付与、SRT統計ログ出力を追加する。

## 注意
試験後は必ず以下で解除する。
```bash
sudo tc qdisc del dev eth0 root
```
