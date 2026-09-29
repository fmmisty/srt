# RFD USB-AES 2CH ― XMOS XU316 UAC2 ファーム設計メモ（骨子）

対象: 1U USB/AES/アナログ AD/DA インターフェース（PCM1862 ADC / PCM5242 DAC / AES3 トランシーバ / XMOS **XU316**）。
Raspberry Pi 4（または PC）へ **USB Audio Class 2.0（ドライバ不要）** で見せる。Pi 側は `srt_send`/`srt_recv`/`codec_web.py`。

> 実装は XMOS 公式 **`sw_usb_audio`（`lib_xua`）** リファレンスをベースに、`xua_conf.h` の定義で構成するのが最短。本メモはその設定方針。実ビルドには XMOS XTC Tools（xcommon-cmake）と実機が必要。

## 1. USB で見せるチャンネル構成（推奨）

Pi から「アナログ入力ペア」と「AES入力ペア」を選べるよう、**4in / 4out** で出す:

| USB ch | 方向 device←host (OUT) | 方向 device→host (IN) |
|---|---|---|
| 1-2 | アナログ OUT（DAC PCM5242） | アナログ IN（ADC PCM1862） |
| 3-4 | AES/EBU OUT（SPDIF TX→トランス） | AES/EBU IN（SPDIF RX←トランス） |

- Pi 側 ALSA では 4ch デバイスとして見える。`srt_send` は入力ペアを選ぶ（ch1-2=analog / ch3-4=AES）、`srt_recv` は出したいペアへ（両方同時出しも可）。
- ※電気的に **AES3 = S/PDIF（プロ・110Ω・トランス絶縁）** なので、XMOS では `lib_spdif` の SPDIF TX/RX をそのまま使う。

## 2. クロック方針（要決定・重要）

- ローカル基準: **24.576MHz（48/96k系）/ 22.5792MHz（44.1系）**。デバイスを**マスタ**にして DAC・ADC・AES OUT を同期。
- **AES IN** は SPDIF RX が入力クロックを回復する＝外部同期。ローカルマスタと非同期になり得るので、**AES IN 経路に ASRC（非同期サンプルレート変換）を入れる**のが安全（放送で外部AESソースを受ける想定）。
  - 代替: 「AES IN をワードクロック基準にしてデバイス側を従属」も可だが、複数ソース同時運用が難しくなる。→ **ASRC推奨**。

## 3. XU316 タイル/ポート割り当て（目安）

- tile[0]: USB(xud)、制御(I2C: PCM1862/PCM5242設定)、前面OLED/LED・REMOTE GPIO
- tile[1]: I2S（ADC/DAC）、SPDIF TX/RX（AES）、MCLK 入力、（必要なら ASRC タスク）
- MCLK: 外部 24.576/22.5792 を切替（またはローカルPLL）。`lib_xua` の clockgen に接続。

## 4. 制御・表示

- **PCM1862/PCM5242 初期化**: `AudioHwInit()`/`AudioHwConfig()` を I2C で実装（ゲイン、フォーマット I2S 24bit、サンプルレート追従）。
- **前面表示**: USB LINK、AES LOCK（SPDIF RX ロック状態）、ANALOG IN/OUT の SIG/CLIP、OLED（サンプルレート・dBFS）。メータは XUA の user サンプルフックでピーク検出→OLED/LED タスクへ。
- **REMOTE/GPIO（D-sub9: PTT IN/OUT・GPI1/2・GPO1/2・+5V）**: どちらで扱うか要決定。
  - 案A: XMOS GPIO で受けて USB HID/ベンダ制御で Pi へ通知（低遅延）。
  - 案B: Pi の GPIO に直結（本I/FはUSBオーディオに専念）。← 本機に LAN 無し=Pi外付け構成なら、PTT等は Pi 側 GPIO が素直。
  - いずれも外部接点はフォトカプラ/オープンドレインで絶縁。

## 5. 実装ステップ

1. XMOS `sw_usb_audio` を XU316 ボード構成で clone、`xua_conf.h`（本フォルダのスケルトン）を適用。
2. I2S を PCM1862(ADC)/PCM5242(DAC) に接続（24bit I2S、MCLK 供給）。`AudioHwInit` を I2C で実装。
3. `lib_spdif` の TX/RX を AES トランシーバ段（トランス絶縁）に接続。SPDIF RX ロック→AES LOCK 表示。
4. （推奨）AES IN に ASRC を挿入しローカルクロックへ載せ替え。
5. 4in/4out で UAC2 列挙 → Pi の `arecord -l`/`aplay -l` で確認、`srt_send`/`srt_recv` に接続。
6. 前面 OLED/LED・REMOTE を接続。

## 6. Pi 側との接続（確認事項）

- Pi から見た本機: UAC2 4in/4out。`arecord -D plughw:CARD,0 -f S24_LE(or S16_LE) -r 48000 -c 4` の 4ch のうち欲しいペアを使う（または plug でマッピング）。
- `srt_send` は 2ch を想定 → ALSA の `plug`/`route` で対象ペア(ch1-2 or ch3-4)を 2ch に落とす、もしくは XUA 側で「選択済み2ch」を USB IN に出す簡易版も可（切替は本機制御 or Pi 側）。
- サンプルレート/ビット: 送受と本機を 48kHz で統一（24bit 運用なら srt 側の PCM 形式も合わせる。現状 srt は S16LE なので、当面 16bit で揃えるのが簡単）。

> 注意: 本メモは設計方針。実ファーム(.xc/.c、CMake)は XMOS 実機・XTC Tools で作成・検証が必要。次段として `xua_conf.h` の各定義を実ボードのポートに合わせて確定する。
