# HDIP-3000V を SRT で伝送する（透過トンネル）使い方

HDIP-3000V（Linux/ALSA・RTP/UDP・コーデックは装置側で選択）の音声を、NTTひかり
電話（データコネクト）に依存せず、**一般IP回線（VPN/専用線/インターネット/Starlink）**
で双方向伝送するための手順書。RTPパケットを無改変のまま、その頭にSRTを被せて運ぶ
（＝透過トンネル）。コーデック選択・音質は装置側のまま、SRTがQoSの代わりに
ARQ＋バッファでベストエフォート回線を成立させる。

---

## 1. 用意するもの（各拠点）

- Raspberry Pi 4（2GBで十分）または Linux機
- HDIP-3000V 本体（IP接続モードに設定できること）
- 拠点間の到達手段：**ヤマハRTX等の拠点間VPN**（推奨。ポート開放不要）／固定IP＋ポート開放 のいずれか

## 2. ビルド（各Pi）

```bash
tar xzf srt_audio_final_tunnel_YYYYMMDD.tar.gz
cd srt_audio_final
sudo apt update
sudo apt install -y build-essential git libsrt-openssl-dev libfdk-aac-dev libopus-dev
make            # udp2srt / srt2udp / srt_send / srt_recv などが生成される
```

※透過トンネル（udp2srt/srt2udp）だけなら libsrt だけで動く。fdk-aac/opus は
自作エンコード版（srt_send/srt_recv）を使う場合のみ必要。

## 3. HDIP-3000V 側の設定（各拠点）

- **接続モード**：IP接続モード
- **音声コーデック**：回線帯域に合わせて選択
  - 2M対称回線 → **CLEAR（推奨・可逆1M）**、余裕を見るなら Opus/SBADPCM(0.5M)、無劣化なら LPCM(1.5M・余裕約20%)
- **RTP送信先**：同じ拠点の Pi のIPアドレス、ポート `15000`（装置のRTPポートに合わせる）
- **RTP受信ポート**：`15000`

## 4. トンネル起動（各拠点で1コマンド）

両拠点で同じスクリプトを、**tx-port と rx-port を入れ替えて**実行する。

```bash
# A拠点
./duplex_tunnel.sh --peer=<B拠点IP> --tx-port=9000 --rx-port=9001 \
    --dev-port=15000 --dev-ip=<A拠点の装置IP> --latency=250 --passphrase=<合言葉>

# B拠点
./duplex_tunnel.sh --peer=<A拠点IP> --tx-port=9001 --rx-port=9000 \
    --dev-port=15000 --dev-ip=<B拠点の装置IP> --latency=250 --passphrase=<合言葉>
```

- `--peer` … 相手拠点のIP（VPN内ならVPN側プライベートIP）
- `--tx-port`（自分のSRT待受）と相手の `--rx-port` を一致させる（A:9000↔B:9000、B:9001↔A:9001）
- `--dev-port` … 装置のRTP送受ポート（既定15000）
- `--dev-ip` … 自拠点の装置IP（受信を配る先。Piと装置が同一なら127.0.0.1）
- `--passphrase` … 暗号化（10〜79字、両拠点同一）。VPN内なら省略可
- 停止：Ctrl+C

## 5. 回線帯域の目安（片方向）

| 装置コーデック | 音声 | SRT実効 | 2M対称回線 |
|---|---|---|---|
| LPCM | 1536k | 約1.6M | 入る（余裕約20%） |
| CLEAR（可逆） | 1000k | 約1.15M | 余裕あり（推奨） |
| Opus / SBADPCM | 500k | 約0.6M | 大余裕 |
| G.722 / G.711 | 64〜90k | 約0.1M | 全く問題なし |

- ベストエフォート回線は再送余地を残すため CLEAR か Opus 推奨（LPCMは避ける）。
- `--latency` の目安：局内/専用線 20〜60ms、良好なネット 150〜200ms、
  ベストエフォート/Starlink 250〜400ms。

## 6. 疎通確認・トラブル時

- まず**同一LAN**で A↔B を繋いで音が通ることを確認 → 次にVPN越し。
- 音が出ない：装置のRTP送信先がPi:15000になっているか、`--dev-ip/--dev-port` が
  装置と一致しているか、`--tx/--rx` の対応（相手と入れ替え）を確認。
- 片方向だけ出る：その方向の tx/rx ポート対応、VPN到達、装置の受信ポートを確認。
- ログ：各プロセスが標準エラーに接続状況・中継実績を出力する。

## 7. 参考：他の同梱ツール

- `udp2srt` / `srt2udp` … トンネルの送り/受け単体（duplex_tunnel.sh が内部で使用）
- `duplex_bridge.sh` … 同じ透過運搬を公式ツール srt-live-transmit で行う版
  （`sudo apt install srt-tools` が必要）
- `srt_send` / `srt_recv`（+ `duplex.sh` / `duplex_udp.sh`）
  … SRT側で再圧縮したい場合の自作エンコード版（AAC/aptX/aptX HD/Opus/LPCM選択）。
  HDIP-3000Vのように装置が既にエンコード済みなら**使わない**（二重圧縮になる）。
- `smoke_test.sh` / `udp_test.sh` / `toneutil` … 動作検証用。

---

## 8. HDIP-3000V「IP接続モード」を SRT化する（hdip_srt_bridge）

上の透過トンネルは「送信先が固定のUDP」向け。HDIP-3000Vの**IP接続モード**は、呼制御
(UDP23500・ASCIIテキスト)の中で **MEDIA-ADDR / CONTACT-ADDR** により RTP 送信先を
動的に決めるため、透過トンネルだけでは RTP がトンネルを通らない。そこで **CONTACT/
MEDIA-ADDR を手元BridgeのIPに書き換える ALG 付きブリッジ** `hdip_srt_bridge` を使う。
（音声のみ。映像V-PORT/制御C-PORT/インカムI-PORTは運ばない）

```
HDIP-A ⇄ [Bridge-A] ══SRT(23500制御+15000RTP)══ [Bridge-B] ⇄ HDIP-B
         各Bridgeが隣HDIPへ渡す電文の SRC/CONTACT/MEDIA-ADDR を自分のIPに書換
```

各拠点で（tx/rx を入れ替えて）:
```bash
# A拠点
./hdip_srt_bridge --peer=<B側BridgeのグローバルIP> --tx-port=9000 --rx-port=9001 \
    --local-ip=<A-BridgeのHDIP側IP> --dev-ip=<A拠点HDIPのIP> \
    --latency=250 --passphrase=<合言葉>
# B拠点
./hdip_srt_bridge --peer=<A側BridgeのグローバルIP> --tx-port=9001 --rx-port=9000 \
    --local-ip=<B-BridgeのHDIP側IP> --dev-ip=<B拠点HDIPのIP> \
    --latency=250 --passphrase=<合言葉>
```
- `--local-ip` … Bridge が HDIP と同じLANで持つIP（ALGの書き換え先＝HDIPが到達できるアドレス）
- `--dev-ip`   … 自拠点 HDIP のIP（Bridge が制御応答・RTP を届ける先）
- 既定ポート: 制御=23500、音声=15000（`--ctl-port`/`--media-port` で変更可）

**装置側設定**: IP接続モード。発信側の**ダイヤル先(相手IP)を「手元Bridgeのlocal-ip」に
変更**するだけ。着信側は待受のまま。通常の直接接続はダイヤル先を相手IPにすれば従来通り。

**検証済み(WSLループバック)**: CALL電文の SRC/CONTACT/MEDIA-ADDR が Bridge のIPに
正しく書き換わり、RTP(媒体)も往復。透過トンネルと同じ SRT 耐性(ロス10%+遅延80+
ジッタ30まで無欠落)を享受できる。フレッツ+VPN 上で使う想定。
- `udp_proxy`（`gcc -O2 -o udp_proxy udp_proxy.c`）… ロス/遅延/ジッタを与える劣化模擬プロキシ。
- `tunnel_loss_test.sh` … 透過トンネルの劣化耐性リハーサル（`bash tunnel_loss_test.sh`）。
  実測: ロス10%+遅延80ms+ジッタ30ms まで latency 400ms で無欠落。20%超は CLEAR/Opus へ。
