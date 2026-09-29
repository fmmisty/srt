# Step 1: Raspberry Pi での libsrt ビルド手順

対象: Raspberry Pi OS (64bit, Bookworm以降推奨) / Raspberry Pi 4 or 5

## 1. 必要パッケージのインストール

```bash
sudo apt update
sudo apt install -y build-essential cmake tclsh pkg-config \
    libssl-dev git alsa-utils libasound2-dev
```

- `libssl-dev` : SRTのAES暗号化に必要
- `libasound2-dev` : ALSA経由でマイク/スピーカー入出力するために使用

## 2. libsrt のソース取得とビルド

```bash
cd ~
git clone https://github.com/Haivision/srt.git
cd srt
./configure --prefix=/usr/local --enable-encryption
make -j$(nproc)
sudo make install
sudo ldconfig
```

ビルドオプション補足:
- `--enable-encryption` : AES暗号化を有効化(現状は開発検証用に付けておいて損はない)
- Raspberry Pi 4/5 なら `-j4` 程度で数分でビルド完了

## 3. インストール確認

```bash
srt-live-transmit --version
```

バージョン情報が表示されればOK。

## 4. 動作確認(公式サンプルツールでUDP疎通テスト)

送信側 (Pi A, IPアドレス例: 192.168.1.10):
```bash
ffmpeg -re -i test.wav -f mpegts udp://127.0.0.1:9001 &
srt-live-transmit udp://127.0.0.1:9001 srt://:9000 -v
```

受信側 (Pi B):
```bash
srt-live-transmit srt://192.168.1.10:9000 udp://127.0.0.1:9002 -v
ffplay udp://127.0.0.1:9002
```

これで音声(または映像込み)がSRT経由で転送されることを確認できたら、次はStep 2の
自前の音声専用送受信プログラム(raw PCMをSRTソケットで直接やり取り)に進みます。

## 5. 開発用ヘッダ/ライブラリの場所確認

自前コードをビルドする際に必要:

```bash
ls /usr/local/include/srt/srt.h
ls /usr/local/lib/libsrt.*
```

pkg-config が使える場合:
```bash
pkg-config --cflags --libs srt
```

## 6. fdk-aac のビルド(AACエンコード/デコード用)

FM音声をAACで圧縮してSRT転送する場合に使用。libsrtと同様の手順でビルドする。

```bash
cd ~
git clone https://github.com/mstorsjo/fdk-aac.git
cd fdk-aac
mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..
make -j$(nproc)
sudo make install
sudo ldconfig
```

確認:
```bash
ls /usr/local/include/fdk-aac/
ls /usr/local/lib/libfdk-aac*
pkg-config --cflags --libs fdk-aac
```

> **ライセンス注意**: FDK AACは特許が絡むコーデックです。プロトタイプ・研究用途では
> 問題ありませんが、量産・商用配布する場合は特許ライセンス(Via Licensing等)の
> 確認が必要です。

## 7. libfreeaptx (aptX / aptX HD 用)

マルチコーデック版(`srt_send` / `srt_recv`)で aptX / aptX HD を使う場合に必要。
本リポジトリでは `libfreeaptx/` にソースを同梱しており、`freeaptx.c` を各プログラムに
直接コンパイルして取り込むため、別途 `make install` は不要。

同梱していない環境で取得し直す場合:

```bash
cd srt_audio_final
git clone https://github.com/regularhunter/libfreeaptx.git
```

> **ライセンス注意**: libfreeaptx 自体は LGPLv2.1+ だが、aptX の圧縮アルゴリズムは
> Qualcomm の特許対象。社内検証・プロトタイプは問題になりにくいが、実際に納品・
> 販売する製品に組み込むには特許ライセンスの整理が必要。本番は AAC 系が無難。

## 8. マルチコーデック版のビルドと使い方

```bash
make srt_send srt_recv     # あるいは make all で全部
```

送信側(コーデックを切り替えられる):

```bash
# AAC-LC (128kbps), latency 250ms
arecord -f S16_LE -r 48000 -c 2 -t raw | ./srt_send 9000 --codec=aac --bitrate=128000 --latency=250

# aptX (classic, 約384kbps@48k)
arecord -f S16_LE -r 48000 -c 2 -t raw | ./srt_send 9000 --codec=aptx

# aptX HD (約576kbps@48k)
arecord -f S16_LE -r 48000 -c 2 -t raw | ./srt_send 9000 --codec=aptxhd
```

受信側(コーデック指定は不要。CONFIG の codec_id を見て自動判別):

```bash
./srt_recv <送信元IP> 9000 --latency=250 | aplay -f S16_LE -r 48000 -c 2 -t raw
```

送信側の `--codec=` を変えて再起動するだけで、受信側は無変更のまま自動追従する
(CONFIG メッセージを約100ms間隔で再送しているため)。暗号化は両側に
`--passphrase=<10〜79文字>` を付ける。
