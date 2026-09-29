CC = gcc
CFLAGS = -O2 -Wall -Ilibfreeaptx $(shell pkg-config --cflags srt fdk-aac 2>/dev/null || echo -I/usr/local/include)
LDFLAGS_SRT = $(shell pkg-config --libs srt 2>/dev/null || echo -L/usr/local/lib -lsrt)
LDFLAGS_AAC = $(shell pkg-config --libs fdk-aac 2>/dev/null || echo -L/usr/local/lib -lfdk-aac)
LDFLAGS_OPUS = $(shell pkg-config --libs opus 2>/dev/null || echo -lopus)

# libfreeaptx はソース同梱でビルドに含める(外部インストール不要)
FREEAPTX_SRC = libfreeaptx/freeaptx.c

# コーデック抽象化レイヤの実装。圧縮方式を足す時はここに codec_xxx.c を1つ追加し、
# codec_reg.c の一覧に登録する(srt_send.c/srt_recv.c は無変更)。
CODEC_SRC = codec_reg.c codec_aac.c codec_aptx.c codec_opus.c codec_lpcm.c
CODEC_HDR = srt_codec.h srt_codec_api.h

# マルチコーデック版(本命): srt_send / srt_recv
# 従来の単一コーデック版(aac_*, audio_*)もそのまま残す
all: srt_send srt_recv udp2srt srt2udp hdip_srt_bridge audio_srt_send audio_srt_recv aac_srt_send aac_srt_recv toneutil

# 透過トンネル(コーデック非依存): 装置のUDP/RTPを中身を触らずSRTで運ぶ
udp2srt: udp2srt.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT)

srt2udp: srt2udp.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT)

# HDIP-3000V IP接続モード用 SRTブリッジ(音声のみ・CONTACT/MEDIA-ADDR書換ALG付き)
# 自社利用のためライセンス検証なし(SRT ON/OFFはHDIPのWEB設定のenableで決まる)。
hdip_srt_bridge: hdip_srt_bridge.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT) -lpthread

srt_send: srt_send.c $(CODEC_SRC) $(CODEC_HDR) $(FREEAPTX_SRC)
	$(CC) $(CFLAGS) -o $@ srt_send.c $(CODEC_SRC) $(FREEAPTX_SRC) $(LDFLAGS_SRT) $(LDFLAGS_AAC) $(LDFLAGS_OPUS) -lm -lpthread

srt_recv: srt_recv.c $(CODEC_SRC) $(CODEC_HDR) $(FREEAPTX_SRC)
	$(CC) $(CFLAGS) -o $@ srt_recv.c $(CODEC_SRC) $(FREEAPTX_SRC) $(LDFLAGS_SRT) $(LDFLAGS_AAC) $(LDFLAGS_OPUS) -lm

audio_srt_send: audio_srt_send.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT)

audio_srt_recv: audio_srt_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT)

aac_srt_send: aac_srt_send.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT) $(LDFLAGS_AAC) -lm -lpthread

aac_srt_recv: aac_srt_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT) $(LDFLAGS_AAC) -lm

# 依存なしテスト用ツール(トーン生成/周波数検査)
toneutil: toneutil.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f srt_keygen srt_send srt_recv udp2srt srt2udp hdip_srt_bridge audio_srt_send audio_srt_recv aac_srt_send aac_srt_recv toneutil

.PHONY: all clean keygen

# SRTオプションキー発行ツール(社内専用・allに含めない/客先に配らない)
keygen: srt_keygen
srt_keygen: srt_keygen.c srt_license.h srt_license_pub.h
	$(CC) $(CFLAGS) -o $@ $< -lcrypto
