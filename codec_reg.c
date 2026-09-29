/*
 * codec_reg.c  --  コーデックの一覧と検索。
 * 新しい圧縮方式を足す時は、extern を1行と g_codecs[] への登録を1行足すだけ。
 */
#include "srt_codec_api.h"
#include <string.h>
#include <stddef.h>

extern const codec_t codec_aac;
extern const codec_t codec_aptx;
extern const codec_t codec_aptxhd;
extern const codec_t codec_opus;
extern const codec_t codec_lpcm;

/* ここに並べたものが利用可能な全コーデック。 */
static const codec_t *const g_codecs[] = {
    &codec_aac,
    &codec_aptx,
    &codec_aptxhd,
    &codec_opus,
    &codec_lpcm,
};
static const int g_n = (int)(sizeof(g_codecs) / sizeof(g_codecs[0]));

const codec_t *codec_get_by_id(int id) {
    for (int i = 0; i < g_n; i++) if (g_codecs[i]->id == id) return g_codecs[i];
    return NULL;
}
const codec_t *codec_get_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_n; i++) if (strcmp(g_codecs[i]->name, name) == 0) return g_codecs[i];
    return NULL;
}
