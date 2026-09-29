/*
 * codec_aac.c  --  AAC-LC (fdk-aac) の codec_t 実装。
 *
 * 検証済みの srt_send.c / srt_recv.c の AAC 部分を抽象化APIに切り出したもの。
 * TT_MP4_RAW / AAC-LC / AFTERBURNER 有効。ASC は CONFIG のコーデック固有部で運ぶ。
 */
#include "srt_codec_api.h"
#include <fdk-aac/aacenc_lib.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== エンコーダ ===== */
typedef struct {
    HANDLE_AACENCODER h;
    int pcm_samples_per_frame;   /* frameLength * channels */
    int frame_bytes;
    unsigned int max_out;
} aac_enc_t;

static void *aac_enc_open(int samplerate, int channels, int bitrate,
                          int *frame_bytes, unsigned char *cfg, int *cfg_len) {
    aac_enc_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    if (aacEncOpen(&e->h, 0, channels) != AACENC_OK) { free(e); return NULL; }
    aacEncoder_SetParam(e->h, AACENC_AOT, AOT_AAC_LC);
    aacEncoder_SetParam(e->h, AACENC_SAMPLERATE, samplerate);
    aacEncoder_SetParam(e->h, AACENC_CHANNELMODE, channels == 2 ? MODE_2 : MODE_1);
    aacEncoder_SetParam(e->h, AACENC_CHANNELORDER, 1);
    aacEncoder_SetParam(e->h, AACENC_BITRATE, bitrate);
    aacEncoder_SetParam(e->h, AACENC_TRANSMUX, TT_MP4_RAW);
    aacEncoder_SetParam(e->h, AACENC_AFTERBURNER, 1);
    if (aacEncEncode(e->h, NULL, NULL, NULL, NULL) != AACENC_OK) { aacEncClose(&e->h); free(e); return NULL; }
    AACENC_InfoStruct info;
    if (aacEncInfo(e->h, &info) != AACENC_OK) { aacEncClose(&e->h); free(e); return NULL; }

    e->pcm_samples_per_frame = info.frameLength * channels;
    e->frame_bytes = e->pcm_samples_per_frame * (int)sizeof(INT_PCM);
    e->max_out = info.maxOutBufBytes;
    if ((int)info.confSize > MAX_CONFIG_SIZE) { aacEncClose(&e->h); free(e); return NULL; }
    memcpy(cfg, info.confBuf, info.confSize);
    *cfg_len = (int)info.confSize;
    *frame_bytes = e->frame_bytes;

    fprintf(stderr, "[codec/aac] enc: frameLength=%u ch=%d frame_bytes=%d ASC=%u maxOut=%u bitrate=%d\n",
            info.frameLength, channels, e->frame_bytes, info.confSize, info.maxOutBufBytes, bitrate);
    return e;
}

static void aac_enc_reset(void *st) { (void)st; }   /* AAC-LCフレームは独立。no-op */

static int aac_enc_encode(void *st, const int16_t *pcm, int frame_bytes,
                          unsigned char *out, int out_cap) {
    aac_enc_t *e = st;
    AACENC_BufDesc in = {0}, ou = {0};
    AACENC_InArgs ia = {0}; AACENC_OutArgs oa = {0};
    void *ib = (void *)pcm; int iid = IN_AUDIO_DATA, isz = frame_bytes, iel = sizeof(INT_PCM);
    in.numBufs = 1; in.bufs = &ib; in.bufferIdentifiers = &iid; in.bufSizes = &isz; in.bufElSizes = &iel;
    void *ob = out; int oid = OUT_BITSTREAM_DATA, osz = out_cap, oel = 1;
    ou.numBufs = 1; ou.bufs = &ob; ou.bufferIdentifiers = &oid; ou.bufSizes = &osz; ou.bufElSizes = &oel;
    ia.numInSamples = e->pcm_samples_per_frame;
    if (aacEncEncode(e->h, &in, &ou, &ia, &oa) != AACENC_OK) return -1;
    return oa.numOutBytes;
}

static void aac_enc_close(void *st) {
    aac_enc_t *e = st; if (!e) return; aacEncClose(&e->h); free(e);
}

/* ===== デコーダ ===== */
typedef struct { HANDLE_AACDECODER h; } aac_dec_t;

static void *aac_dec_open(int samplerate, int channels,
                          const unsigned char *cfg, int cfg_len) {
    (void)samplerate; (void)channels;
    if (cfg_len <= 0 || cfg_len > MAX_CONFIG_SIZE) return NULL;
    aac_dec_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->h = aacDecoder_Open(TT_MP4_RAW, 1);
    if (!d->h) { free(d); return NULL; }
    UCHAR *cp[1] = { (UCHAR *)cfg }; UINT cl[1] = { (UINT)cfg_len };
    if (aacDecoder_ConfigRaw(d->h, cp, cl) != AAC_DEC_OK) { aacDecoder_Close(d->h); free(d); return NULL; }
    return d;
}

static int aac_dec_decode(void *st, const unsigned char *in, int in_len,
                          int16_t *pcm_out, int pcm_cap_bytes) {
    aac_dec_t *d = st;
    UCHAR *ip[1] = { (UCHAR *)in }; UINT il[1] = { (UINT)in_len }; UINT valid = (UINT)in_len;
    if (aacDecoder_Fill(d->h, ip, il, &valid) != AAC_DEC_OK) return -1;
    AAC_DECODER_ERROR e = aacDecoder_DecodeFrame(d->h, pcm_out, pcm_cap_bytes / (int)sizeof(INT_PCM), 0);
    if (e == AAC_DEC_NOT_ENOUGH_BITS) return 0;
    if (e != AAC_DEC_OK) return -1;
    CStreamInfo *si = aacDecoder_GetStreamInfo(d->h);
    return si->frameSize * si->numChannels * (int)sizeof(INT_PCM);
}

static void aac_dec_close(void *st) {
    aac_dec_t *d = st; if (!d) return; aacDecoder_Close(d->h); free(d);
}

const codec_t codec_aac = {
    .id = CODEC_AAC, .name = "aac",
    .enc_open = aac_enc_open, .enc_reset = aac_enc_reset,
    .enc_encode = aac_enc_encode, .enc_close = aac_enc_close,
    .dec_open = aac_dec_open, .dec_decode = aac_dec_decode, .dec_close = aac_dec_close,
};
