CC = gcc
CFLAGS = -O2 -Wall -Ilibfreeaptx $(shell pkg-config --cflags srt fdk-aac 2>/dev/null || echo -I/usr/local/include)
LDFLAGS_SRT = $(shell pkg-config --libs srt 2>/dev/null || echo -L/usr/local/lib -lsrt)
LDFLAGS_AAC = $(shell pkg-config --libs fdk-aac 2>/dev/null || echo -L/usr/local/lib -lfdk-aac)

# libfreeaptx はソース同梱でビルドに含める(外部インストール不要)
FREEAPTX_SRC = libfreeaptx/freeaptx.c

# マルチコーデック版(本命): srt_send / srt_recv
# 従来の単一コーデック版(aac_*, audio_*)もそのまま残す
all: srt_send srt_recv audio_srt_send audio_srt_recv aac_srt_send aac_srt_recv

srt_send: srt_send.c srt_codec.h $(FREEAPTX_SRC)
	$(CC) $(CFLAGS) -o $@ srt_send.c $(FREEAPTX_SRC) $(LDFLAGS_SRT) $(LDFLAGS_AAC) -lm -lpthread

srt_recv: srt_recv.c srt_codec.h $(FREEAPTX_SRC)
	$(CC) $(CFLAGS) -o $@ srt_recv.c $(FREEAPTX_SRC) $(LDFLAGS_SRT) $(LDFLAGS_AAC) -lm

audio_srt_send: audio_srt_send.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT)

audio_srt_recv: audio_srt_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT)

aac_srt_send: aac_srt_send.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT) $(LDFLAGS_AAC) -lm -lpthread

aac_srt_recv: aac_srt_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_SRT) $(LDFLAGS_AAC) -lm

clean:
	rm -f srt_send srt_recv audio_srt_send audio_srt_recv aac_srt_send aac_srt_recv

.PHONY: all clean
