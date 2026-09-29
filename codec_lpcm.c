/*
 * codec_lpcm.c  --  LPCM(無圧縮リニアPCM)の codec_t 実装。
 *
 * 圧縮せず S16LE stereo をそのまま流す。エンコード/デコードとも単純コピー。
 * 有線LAN・専用線など帯域に余裕がある環境向けの最高音質・最小処理遅延の選択肢。
 * ビットレートは 48kHz/16bit/stereo = 1,536kbps 固定(圧縮比 1:1)。
 *
 * SRTライブモードの1メッセージ上限(1316byte)に収めるため、1フレームを
 * LPCM_FRAME_SAMPLES(256サンプル/ch)= 1024byte に区切って送る。追加設定は無し。
 */
#include "srt_codec_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int channels;
    int frame_bytes;   /* LPCM_FRAME_SAMPLES * channels * sizeof(int16) */
} lpcm_t;

/* ===== エンコーダ ===== */
static void *lpcm_enc_open(int samplerate, int channels, int bitrate,
                           int *frame_bytes, unsigned char *cfg, int *cfg_len) {
    (void)bitrate; (void)cfg;
    lpcm_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->channels = channels;
    e->frame_bytes = LPCM_FRAME_SAMPLES * channels * (int)sizeof(int16_t);
    *cfg_len = 0;                 /* 追加設定なし(sr/chはCONFIGにある) */
    *frame_bytes = e->frame_bytes;
    fprintf(stderr, "[codec/lpcm] enc: %dch %dHz 無圧縮 frame=%dサンプル/ch frame_bytes=%d "
                    "(%dkbps相当)\n",
            channels, samplerate, LPCM_FRAME_SAMPLES, e->frame_bytes,
            samplerate * channels * 16 / 1000);
    return e;
}

static void lpcm_enc_reset(void *st) { (void)st; }  /* 状態なし */

static int lpcm_enc_encode(void *st, const int16_t *pcm, int frame_bytes,
                           unsigned char *out, int out_cap) {
    lpcm_t *e = st;
    if (frame_bytes != e->frame_bytes) return -1;
    if (frame_bytes > out_cap) return -1;
    memcpy(out, pcm, frame_bytes);
    return frame_bytes;
}

static void lpcm_enc_close(void *st) { free(st); }

/* ===== デコーダ ===== */
static void *lpcm_dec_open(int samplerate, int channels,
                           const unsigned char *cfg, int cfg_len) {
    (void)samplerate; (void)cfg; (void)cfg_len;
    lpcm_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->channels = channels;
    return d;
}

static int lpcm_dec_decode(void *st, const unsigned char *in, int in_len,
                           int16_t *pcm_out, int pcm_cap_bytes) {
    (void)st;
    if (in_len > pcm_cap_bytes) return -1;
    memcpy(pcm_out, in, in_len);
    return in_len;
}

static void lpcm_dec_close(void *st) { free(st); }

const codec_t codec_lpcm = {
    .id = CODEC_LPCM, .name = "lpcm",
    .enc_open = lpcm_enc_open, .enc_reset = lpcm_enc_reset,
    .enc_encode = lpcm_enc_encode, .enc_close = lpcm_enc_close,
    .dec_open = lpcm_dec_open, .dec_decode = lpcm_dec_decode, .dec_close = lpcm_dec_close,
};
