/*
 * srt_codec_api.h
 *
 * コーデックの抽象化インターフェース(vtable)。
 * srt_send / srt_recv 本体はこの codec_t 越しにエンコード/デコードを呼ぶので、
 * コーデック非依存になっている。
 *
 * ★ 新しい圧縮方式を追加する手順(これだけ):
 *   1. codec_xxx.c を作り、下の codec_t を1つ実装して extern で公開する
 *   2. srt_codec.h の codec_id( CODEC_XXX )を1つ追加する
 *   3. codec_reg.c の一覧に &codec_xxx を1行足す
 *   → srt_send.c / srt_recv.c は一切変更不要。
 *
 * ワイヤプロトコル・PCM形式・S16<->S24変換は srt_codec.h を参照(共通)。
 * PCMは全コーデック共通で S16LE インターリーブ stereo。
 */
#ifndef SRT_CODEC_API_H
#define SRT_CODEC_API_H

#include <stdint.h>
#include "srt_codec.h"   /* CODEC_* id, s16<->s24 変換, put/get_u32le 等 */

typedef struct codec_s {
    int         id;     /* CODEC_AAC / CODEC_APTX / CODEC_APTXHD ... */
    const char *name;   /* CLIの --codec= 名: "aac" / "aptx" / "aptxhd" */

    /* ---- エンコーダ ---- */

    /* エンコーダを開く。失敗時 NULL。
       *frame_bytes … 1回の enc_encode に渡す S16LE PCM 1フレームのバイト数。
       cfg, *cfg_len … CONFIGメッセージのコーデック固有部に載せる設定バイト列
                      (AAC=ASC, aptX=0バイト)。呼び出し側が MAX_CONFIG_SIZE の
                      バッファを渡す。 */
    void *(*enc_open)(int samplerate, int channels, int bitrate,
                      int *frame_bytes,
                      unsigned char *cfg, int *cfg_len);

    /* 接続開始時に内部状態(予測器等)を初期化し、まっさらなデコーダと揃える。
       フレーム独立なコーデックでは no-op。 */
    void (*enc_reset)(void *st);

    /* PCM 1フレームをエンコードして out(容量 out_cap)へ書く。
       返り値=書き込みバイト数(>=0)、エラー時 -1。0 は「今回出力なし」。 */
    int (*enc_encode)(void *st, const int16_t *pcm, int frame_bytes,
                      unsigned char *out, int out_cap);

    void (*enc_close)(void *st);

    /* ---- デコーダ ---- */

    /* デコーダを開く(接続ごとに作り直す)。cfg は送信側 enc_open が出した
       コーデック固有設定(codec_id や sr/ch を除いた部分)。失敗時 NULL。 */
    void *(*dec_open)(int samplerate, int channels,
                      const unsigned char *cfg, int cfg_len);

    /* エンコード済みペイロード1個をデコードして pcm_out へ S16LE で書く。
       pcm_cap_bytes は pcm_out のバイト容量。
       返り値=書き込んだPCMバイト数(>=0)、エラー時 -1。0 は「出力なし」。 */
    int (*dec_decode)(void *st, const unsigned char *in, int in_len,
                      int16_t *pcm_out, int pcm_cap_bytes);

    void (*dec_close)(void *st);
} codec_t;

/* id / 名前で検索。見つからなければ NULL。 */
const codec_t *codec_get_by_id(int id);
const codec_t *codec_get_by_name(const char *name);

#endif /* SRT_CODEC_API_H */
