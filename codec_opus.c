/*
 * codec_opus.c  --  Opus (libopus) の codec_t 実装。
 *
 * 低レイテンシ・高音質で音声IP伝送の定番。STL用途に好適。
 * Opus は追加のコーデック設定ヘッダを持たず、デコーダはサンプルレートと
 * チャンネル数(CONFIGのsr/chから取得)だけで開ける。フレーム長はパケットから
 * 自動判定されるので、CONFIG固有部は0バイト。
 *
 * フレーム長は 480サンプル/ch = 10ms @48kHz(Opusの有効フレーム長)。低遅延。
 * 出力パケットは通常数百バイトで、SRTライブモード上限(1316)に十分収まる。
 *
 * ビルドに libopus が必要: apt install libopus-dev (Red Pitaya等でも apt で入る)。
 */
#include "srt_codec_api.h"
#include <opus/opus.h>
#include <stdio.h>
#include <stdlib.h>

/* Opus の1フレーム長(1chあたりサンプル数)。48kHzで有効な値: 120/240/480/960/1920/2880。
   480 = 10ms。低遅延と効率のバランス。 */
#define OPUS_FRAME_SAMPLES 480

/* ===== エンコーダ ===== */
typedef struct {
    OpusEncoder *enc;
    int channels;
    int frame_bytes;     /* OPUS_FRAME_SAMPLES * channels * sizeof(int16) */
} opus_enc_t;

static void *opus_enc_open(int samplerate, int channels, int bitrate,
                           int *frame_bytes, unsigned char *cfg, int *cfg_len) {
    (void)cfg;
    int err = 0;
    opus_enc_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->channels = channels;
    e->enc = opus_encoder_create(samplerate, channels, OPUS_APPLICATION_AUDIO, &err);
    if (!e->enc || err != OPUS_OK) { free(e); return NULL; }
    opus_encoder_ctl(e->enc, OPUS_SET_BITRATE(bitrate));

    e->frame_bytes = OPUS_FRAME_SAMPLES * channels * (int)sizeof(opus_int16);
    *cfg_len = 0;                 /* Opusは追加設定なし(sr/chはCONFIGにある) */
    *frame_bytes = e->frame_bytes;
    fprintf(stderr, "[codec/opus] enc: %dch %dHz %dbps frame=%dサンプル/ch(10ms) frame_bytes=%d\n",
            channels, samplerate, bitrate, OPUS_FRAME_SAMPLES, e->frame_bytes);
    return e;
}

static void opus_enc_reset(void *st) {
    opus_enc_t *e = st;
    opus_encoder_ctl(e->enc, OPUS_RESET_STATE);   /* 新しい接続に合わせて状態初期化 */
}

static int opus_enc_encode(void *st, const int16_t *pcm, int frame_bytes,
                           unsigned char *out, int out_cap) {
    opus_enc_t *e = st;
    if (frame_bytes != e->frame_bytes) return -1;
    int n = opus_encode(e->enc, (const opus_int16 *)pcm, OPUS_FRAME_SAMPLES, out, out_cap);
    if (n < 0) { fprintf(stderr, "[codec/opus] opus_encodeエラー: %s\n", opus_strerror(n)); return -1; }
    return n;
}

static void opus_enc_close(void *st) {
    opus_enc_t *e = st; if (!e) return;
    if (e->enc) opus_encoder_destroy(e->enc);
    free(e);
}

/* ===== デコーダ ===== */
typedef struct {
    OpusDecoder *dec;
    int channels;
} opus_dec_t;

static void *opus_dec_open(int samplerate, int channels,
                           const unsigned char *cfg, int cfg_len) {
    (void)cfg; (void)cfg_len;
    int err = 0;
    opus_dec_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->channels = channels;
    d->dec = opus_decoder_create(samplerate, channels, &err);
    if (!d->dec || err != OPUS_OK) { free(d); return NULL; }
    return d;
}

static int opus_dec_decode(void *st, const unsigned char *in, int in_len,
                           int16_t *pcm_out, int pcm_cap_bytes) {
    opus_dec_t *d = st;
    int max_per_ch = pcm_cap_bytes / (d->channels * (int)sizeof(opus_int16));
    int nsamp = opus_decode(d->dec, in, in_len, (opus_int16 *)pcm_out, max_per_ch, 0);
    if (nsamp < 0) { fprintf(stderr, "[codec/opus] opus_decodeエラー: %s\n", opus_strerror(nsamp)); return -1; }
    return nsamp * d->channels * (int)sizeof(opus_int16);
}

static void opus_dec_close(void *st) {
    opus_dec_t *d = st; if (!d) return;
    if (d->dec) opus_decoder_destroy(d->dec);
    free(d);
}

const codec_t codec_opus = {
    .id = CODEC_OPUS, .name = "opus",
    .enc_open = opus_enc_open, .enc_reset = opus_enc_reset,
    .enc_encode = opus_enc_encode, .enc_close = opus_enc_close,
    .dec_open = opus_dec_open, .dec_decode = opus_dec_decode, .dec_close = opus_dec_close,
};
