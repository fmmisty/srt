/*
 * srt_codec.h
 *
 * srt_send / srt_recv 共通のプロトコル定義とユーティリティ。
 *
 * マルチコーデック対応のためのメッセージ形式:
 *   先頭1byte = 種別タグ
 *     0x01 = CONFIG
 *     0x02 = AUDIO
 *
 *   CONFIG ペイロード(タグの次から):
 *     [0]      codec_id  (0=AAC-LC, 1=aptX, 2=aptX HD)
 *     [1..4]   samplerate (uint32, リトルエンディアン)  ※受信側の情報表示用
 *     [5]      channels
 *     [6..]    コーデック固有設定
 *                AAC : ASC(AudioSpecificConfig) バイト列
 *                aptX/aptX HD : なし(0バイト)
 *
 *   AUDIO ペイロード(タグの次から):
 *     コーデックフレーム本体(AACフレーム / aptXバイト列)
 *
 * ---- aptX の PCM 形式について ----
 * libfreeaptx は 24bit signed(S24LE) インターリーブstereoを入出力とする。
 * 本プロジェクトの stdin/stdout は S16LE stereo なので、送信側で S16→S24、
 * 受信側で S24→S16 に変換する(ゼロ拡張/上位16bit切り出し)。
 */

#ifndef SRT_CODEC_H
#define SRT_CODEC_H

#include <stdint.h>
#include <string.h>

#define MSG_TYPE_CONFIG 0x01
#define MSG_TYPE_AUDIO  0x02

/* 1メッセージの受信バッファ上限。SRTライブモードの実質上限(1316)より大きく取る。 */
#define MAX_MSG_SIZE    8192
/* CONFIGのコーデック固有部(AACのASC等)の上限。 */
#define MAX_CONFIG_SIZE 64

#define CODEC_AAC    0
#define CODEC_APTX   1
#define CODEC_APTXHD 2
#define CODEC_OPUS   3
#define CODEC_LPCM   4

/* LPCM(無圧縮)送信時の1フレーム stereo サンプル数(1chあたり)。
   S16LE stereo なので 1フレーム = SAMPLES * 2ch * 2byte。
   256 → 256*2*2 = 1024byte (+タグ1 = 1025) で SRTライブモード上限1316に収まる。
   256/48kHz ≒ 5.3ms でレイテンシも小さい。 */
#define LPCM_FRAME_SAMPLES 256

/* aptX 送信時に1フレームとして扱う stereo サンプル数(4の倍数であること)。
   SRTライブモードは1メッセージ最大1316バイト(上限1456)なので、最も出力の大きい
   aptX HD(6byte/ブロック)でも収まるようにフレーム長を決める必要がある。
   512 stereo サンプル = 128 aptXブロック
     → aptX    128*4 = 512 byte  (+タグ1 = 513)
     → aptX HD 128*6 = 768 byte  (+タグ1 = 769)   … どちらも1316に収まる。
   1024にすると aptX HD が 1536+1=1537 byte となり SRT の
   "Incorrect use of Message API" エラーで送信できないので注意。
   フレーム長 512/48kHz ≒ 10.7ms でレイテンシも低め。 */
#define APTX_FRAME_SAMPLES 512

static inline const char *codec_name(int id) {
    switch (id) {
        case CODEC_AAC:    return "AAC-LC";
        case CODEC_APTX:   return "aptX";
        case CODEC_APTXHD: return "aptX HD";
        default:           return "unknown";
    }
}

/* コーデック名文字列 → codec_id。未知なら -1。 */
static inline int codec_id_from_name(const char *s) {
    if (strcmp(s, "aac") == 0)    return CODEC_AAC;
    if (strcmp(s, "aptx") == 0)   return CODEC_APTX;
    if (strcmp(s, "aptxhd") == 0) return CODEC_APTXHD;
    return -1;
}

/* S16LE インターリーブ n_samples 個を S24LE に変換(上位16bitへゼロ拡張)。
   out は n_samples*3 バイト必要。 */
static inline void s16_to_s24(const int16_t *in, unsigned char *out, int n_samples) {
    for (int i = 0; i < n_samples; ++i) {
        int16_t v = in[i];
        out[i*3 + 0] = 0;                       /* 下位8bitは0埋め */
        out[i*3 + 1] = (unsigned char)(v & 0xff);
        out[i*3 + 2] = (unsigned char)((v >> 8) & 0xff);
    }
}

/* S24LE n_samples 個を S16LE に変換(上位16bitを取り出し)。
   out は n_samples 要素必要。 */
static inline void s24_to_s16(const unsigned char *in, int16_t *out, int n_samples) {
    for (int i = 0; i < n_samples; ++i) {
        out[i] = (int16_t)((in[i*3 + 1]) | (in[i*3 + 2] << 8));
    }
}

/* uint32 をリトルエンディアンで書き込む。 */
static inline void put_u32le(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static inline uint32_t get_u32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif /* SRT_CODEC_H */
