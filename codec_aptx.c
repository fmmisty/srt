/*
 * codec_aptx.c  --  aptX / aptX HD (libfreeaptx, ソース同梱) の codec_t 実装。
 *
 * aptX は 24bit signed stereo(LLLRRR..., 3byte/サンプル)を、4ステレオサンプル毎に
 * aptX 4byte / aptX HD 6byte へ符号化する。本プロジェクトのPCMは S16LE なので
 * srt_codec.h の s16_to_s24 / s24_to_s16 で変換する。
 *
 * フレーム長 APTX_FRAME_SAMPLES(=512, srt_codec.h) は、最大出力の aptX HD でも
 * SRTライブモードの1メッセージ上限(1316byte)に収まるよう選んである。
 * 予測器を持つ連続ストリーム型なので、接続開始時に enc_reset(=aptx_reset)で揃える。
 * 受信は aptx_decode_sync で欠損時に自動再同期する。
 */
#include "srt_codec_api.h"
#include <freeaptx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== エンコーダ ===== */
typedef struct {
    struct aptx_context *ctx;
    int      hd;
    int      channels;
    int      frame_bytes;   /* S16 PCM 1フレーム */
    int      n_samples;     /* APTX_FRAME_SAMPLES * channels */
    unsigned char *s24;     /* n_samples*3 */
} aptx_enc_t;

static void *aptx_enc_open_impl(int hd, int samplerate, int channels, int bitrate,
                                int *frame_bytes, unsigned char *cfg, int *cfg_len) {
    (void)samplerate; (void)bitrate; (void)cfg;
    if (channels != 2) {
        fprintf(stderr, "[codec/aptx] エラー: aptX/aptX HD は stereo(2ch)専用(指定=%dch)。\n", channels);
        return NULL;
    }
    aptx_enc_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->hd = hd; e->channels = channels;
    e->ctx = aptx_init(hd);
    if (!e->ctx) { free(e); return NULL; }
    e->n_samples = APTX_FRAME_SAMPLES * channels;
    e->frame_bytes = e->n_samples * (int)sizeof(int16_t);
    e->s24 = malloc(e->n_samples * 3);
    if (!e->s24) { aptx_finish(e->ctx); free(e); return NULL; }

    *cfg_len = 0;               /* aptX は追加設定なし */
    *frame_bytes = e->frame_bytes;
    fprintf(stderr, "[codec/%s] enc: frame=%dサンプル(stereo) frame_bytes=%d 圧縮比≒%s\n",
            hd ? "aptxhd" : "aptx", APTX_FRAME_SAMPLES, e->frame_bytes, hd ? "4:1" : "6:1");
    return e;
}
static void *aptx_enc_open  (int sr,int ch,int br,int*fb,unsigned char*c,int*cl){return aptx_enc_open_impl(0,sr,ch,br,fb,c,cl);}
static void *aptxhd_enc_open(int sr,int ch,int br,int*fb,unsigned char*c,int*cl){return aptx_enc_open_impl(1,sr,ch,br,fb,c,cl);}

static void aptx_enc_reset(void *st) { aptx_enc_t *e = st; aptx_reset(e->ctx); }

static int aptx_enc_encode(void *st, const int16_t *pcm, int frame_bytes,
                           unsigned char *out, int out_cap) {
    aptx_enc_t *e = st;
    if (frame_bytes != e->frame_bytes) return -1;
    s16_to_s24(pcm, e->s24, e->n_samples);
    size_t written = 0;
    size_t processed = aptx_encode(e->ctx, e->s24, (size_t)e->n_samples * 3,
                                   out, (size_t)out_cap, &written);
    if (processed != (size_t)e->n_samples * 3) return -1;
    return (int)written;
}

static void aptx_enc_close(void *st) {
    aptx_enc_t *e = st; if (!e) return;
    if (e->ctx) aptx_finish(e->ctx);
    free(e->s24); free(e);
}

/* ===== デコーダ ===== */
typedef struct {
    struct aptx_context *ctx;
    int      hd;
    int      syncing;
    unsigned char *s24;   /* デコード出力(S24) */
    int      s24_cap;
} aptx_dec_t;

static void *aptx_dec_open_impl(int hd, int samplerate, int channels,
                                const unsigned char *cfg, int cfg_len) {
    (void)samplerate; (void)channels; (void)cfg; (void)cfg_len;
    aptx_dec_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->hd = hd;
    d->ctx = aptx_init(hd);
    if (!d->ctx) { free(d); return NULL; }
    /* 入力最大 MAX_MSG_SIZE を想定(aptX=4byte/サンプル→ *6 で S24、余裕を足す) */
    d->s24_cap = MAX_MSG_SIZE * 6 + 32;
    d->s24 = malloc(d->s24_cap);
    if (!d->s24) { aptx_finish(d->ctx); free(d); return NULL; }
    return d;
}
static void *aptx_dec_open  (int sr,int ch,const unsigned char*c,int cl){return aptx_dec_open_impl(0,sr,ch,c,cl);}
static void *aptxhd_dec_open(int sr,int ch,const unsigned char*c,int cl){return aptx_dec_open_impl(1,sr,ch,c,cl);}

static int aptx_dec_decode(void *st, const unsigned char *in, int in_len,
                           int16_t *pcm_out, int pcm_cap_bytes) {
    aptx_dec_t *d = st;
    size_t written = 0, dropped = 0; int synced = 0;
    aptx_decode_sync(d->ctx, in, (size_t)in_len, d->s24, (size_t)d->s24_cap,
                     &written, &synced, &dropped);
    if (!synced && !d->syncing) {
        fprintf(stderr, "[codec/%s] dec: 同期喪失、再同期中...\n", d->hd ? "aptxhd" : "aptx");
        d->syncing = 1;
    } else if (synced && d->syncing) {
        fprintf(stderr, "[codec/%s] dec: 再同期成功(破棄=%zuバイト)。\n", d->hd ? "aptxhd" : "aptx", dropped);
        d->syncing = 0;
    }
    if (written == 0) return 0;
    int n_samples = (int)(written / 3);
    int pcm_bytes = n_samples * (int)sizeof(int16_t);
    if (pcm_bytes > pcm_cap_bytes) return -1;
    s24_to_s16(d->s24, pcm_out, n_samples);
    return pcm_bytes;
}

static void aptx_dec_close(void *st) {
    aptx_dec_t *d = st; if (!d) return;
    if (d->ctx) aptx_finish(d->ctx);
    free(d->s24); free(d);
}

const codec_t codec_aptx = {
    .id = CODEC_APTX, .name = "aptx",
    .enc_open = aptx_enc_open, .enc_reset = aptx_enc_reset,
    .enc_encode = aptx_enc_encode, .enc_close = aptx_enc_close,
    .dec_open = aptx_dec_open, .dec_decode = aptx_dec_decode, .dec_close = aptx_dec_close,
};
const codec_t codec_aptxhd = {
    .id = CODEC_APTXHD, .name = "aptxhd",
    .enc_open = aptxhd_enc_open, .enc_reset = aptx_enc_reset,
    .enc_encode = aptx_enc_encode, .enc_close = aptx_enc_close,
    .dec_open = aptxhd_dec_open, .dec_decode = aptx_dec_decode, .dec_close = aptx_dec_close,
};
