/*
 * srt_recv.c  ―  マルチコーデック対応 SRT 音声受信
 *
 * SRTソケット経由で音声メッセージを受信し、CONFIG に含まれる codec_id を見て
 * 自動的に AAC-LC / aptX / aptX HD のデコーダを初期化・切り替える。デコード結果は
 * 生PCM(S16LE stereo)として標準出力へ書き出す。コーデック指定は不要。
 *
 * 使い方:
 *   ./srt_recv <send_host> <send_port> [--latency=150] [--passphrase=xxx] \
 *     | aplay -f S16_LE -r 48000 -c 2 -t raw
 *
 * メッセージ形式は srt_codec.h を参照。CONFIG は約100ms間隔で再送されるため、
 * デコーダ初期化前に届いた AUDIO は破棄し、次の CONFIG を待てば自己修復する。
 * 接続が切れても終了せず、自動的に再接続を試み続ける(常駐稼働を想定)。
 */

#include <srt/srt.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <freeaptx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "srt_codec.h"

#define MAX_MSG_SIZE 8192
#define PCM_OUT_SAMPLES 8192      /* AACデコード出力(サンプル数) */
#define MAX_CONFIG_SIZE 64
#define RECONNECT_WAIT_MS 1000

static volatile sig_atomic_t g_should_exit = 0;
static void handle_signal(int sig) { (void)sig; g_should_exit = 1; }

static const char *opt_val(const char *arg, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) == 0 && arg[klen] == '=') return arg + klen + 1;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "使い方: %s <send_host> <send_port> [--latency=150] [--passphrase=xxx]\n",
                argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    int latency_ms = 150;
    const char *passphrase = NULL;

    for (int i = 3; i < argc; ++i) {
        const char *v;
        if      ((v = opt_val(argv[i], "--latency")))    latency_ms = atoi(v);
        else if ((v = opt_val(argv[i], "--passphrase"))) passphrase = v;
        else { fprintf(stderr, "[recv] 不明なオプション: %s\n", argv[i]); return 1; }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    srt_startup();

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "[recv] IPアドレスが不正です: %s\n", host);
        return 1;
    }

    unsigned char msg_buf[MAX_MSG_SIZE];
    INT_PCM pcm_out[PCM_OUT_SAMPLES];
    /* aptX S24 デコード出力: 入力全体+1サンプル分の余裕が必要。
       入力最大 MAX_MSG_SIZE バイト(aptX=4byte/サンプル)→ 出力 MAX_MSG_SIZE/4*24 + 余裕。 */
    unsigned char s24_out[MAX_MSG_SIZE * 6 + 32];
    int16_t s16_out[MAX_MSG_SIZE * 2 + 16];

    long total_frames = 0, total_pcm_bytes = 0, dropped_before_init_total = 0;
    long reconnect_count = 0;

    /* ---- 再接続ループ ---- */
    while (!g_should_exit) {
        SRTSOCKET sock = srt_create_socket();
        if (sock == SRT_INVALID_SOCK) {
            fprintf(stderr, "[recv] srt_create_socketエラー: %s (再試行)\n", srt_getlasterror_str());
            usleep(RECONNECT_WAIT_MS * 1000);
            continue;
        }
        srt_setsockopt(sock, 0, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));

        if (passphrase != NULL) {
            size_t plen = strlen(passphrase);
            if (plen < 10 || plen > 79) {
                fprintf(stderr, "[recv] エラー: パスフレーズは10〜79文字である必要があります"
                                "(現在%zu文字)。\n", plen);
                srt_close(sock); srt_cleanup(); return 1;
            }
            srt_setsockopt(sock, 0, SRTO_PASSPHRASE, passphrase, (int)plen);
            if (reconnect_count == 0) fprintf(stderr, "[recv] 暗号化: 有効\n");
        } else {
            if (reconnect_count == 0) fprintf(stderr, "[recv] 暗号化: 無効(平文で受信します)\n");
        }

        if (reconnect_count == 0)
            fprintf(stderr, "[recv] %s:%d へ接続中 (latency=%dms)...\n", host, port, latency_ms);
        else
            fprintf(stderr, "[recv] %s:%d へ再接続中... (%ld回目)\n", host, port, reconnect_count);

        if (srt_connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
            fprintf(stderr, "[recv] srt_connectエラー: %s (%dms後に再試行)\n",
                    srt_getlasterror_str(), RECONNECT_WAIT_MS);
            srt_close(sock);
            reconnect_count++;
            usleep(RECONNECT_WAIT_MS * 1000);
            continue;
        }
        fprintf(stderr, "[recv] 接続完了。CONFIGを待っています...\n");

        /* ---- デコーダは CONFIG 受信後に初めて開く(接続ごとに作り直す) ---- */
        int cur_codec = -1;
        HANDLE_AACDECODER hDec = NULL;      /* AAC用 */
        struct aptx_context *aptx = NULL;   /* aptX用 */
        int aptx_syncing = 0;
        long dropped_before_init = 0;

        while (!g_should_exit) {
            int n = srt_recv(sock, (char *)msg_buf, MAX_MSG_SIZE);
            if (n == SRT_ERROR) {
                fprintf(stderr, "[recv] 接続切断: %s (再接続します)\n", srt_getlasterror_str());
                break;
            }
            if (n <= 1) continue;

            unsigned char type = msg_buf[0];
            unsigned char *payload = msg_buf + 1;
            int payload_len = n - 1;

            /* ================= CONFIG ================= */
            if (type == MSG_TYPE_CONFIG) {
                if (cur_codec >= 0) continue; /* 初期化済み。定期再送は無視 */
                if (payload_len < 6) {
                    fprintf(stderr, "[recv] 警告: CONFIGが短すぎます(%dバイト)。無視。\n", payload_len);
                    continue;
                }
                int codec       = payload[0];
                uint32_t srate  = get_u32le(payload + 1);
                int channels    = payload[5];

                if (codec == CODEC_AAC) {
                    int asc_len = payload_len - 6;
                    if (asc_len <= 0 || asc_len > MAX_CONFIG_SIZE) {
                        fprintf(stderr, "[recv] 警告: ASCサイズが想定外(%d)。無視。\n", asc_len);
                        continue;
                    }
                    hDec = aacDecoder_Open(TT_MP4_RAW, 1);
                    if (!hDec) { fprintf(stderr, "[recv] aacDecoder_Open失敗。再接続。\n"); break; }
                    UCHAR *conf_ptrs[1] = { payload + 6 };
                    UINT conf_lens[1] = { (UINT)asc_len };
                    AAC_DECODER_ERROR derr = aacDecoder_ConfigRaw(hDec, conf_ptrs, conf_lens);
                    if (derr != AAC_DEC_OK) {
                        fprintf(stderr, "[recv] aacDecoder_ConfigRaw失敗: code=%d\n", derr);
                        aacDecoder_Close(hDec); hDec = NULL;
                        continue;
                    }
                    cur_codec = CODEC_AAC;
                } else if (codec == CODEC_APTX || codec == CODEC_APTXHD) {
                    aptx = aptx_init(codec == CODEC_APTXHD ? 1 : 0);
                    if (!aptx) { fprintf(stderr, "[recv] aptx_init失敗。再接続。\n"); break; }
                    aptx_syncing = 0;
                    cur_codec = codec;
                } else {
                    fprintf(stderr, "[recv] 警告: 未知のcodec_id=%d。無視。\n", codec);
                    continue;
                }

                fprintf(stderr, "[recv] デコーダ初期化完了: codec=%s, sr=%u, ch=%d。"
                                "破棄した音声フレーム数=%ld\n",
                        codec_name(cur_codec), srate, channels, dropped_before_init);
                continue;
            }

            /* ================= AUDIO ================= */
            if (type == MSG_TYPE_AUDIO) {
                if (cur_codec < 0) { dropped_before_init++; continue; }

                if (cur_codec == CODEC_AAC) {
                    UCHAR *in_ptrs[1] = { payload };
                    UINT in_lens[1] = { (UINT)payload_len };
                    UINT bytes_valid = (UINT)payload_len;
                    AAC_DECODER_ERROR derr = aacDecoder_Fill(hDec, in_ptrs, in_lens, &bytes_valid);
                    if (derr != AAC_DEC_OK) {
                        fprintf(stderr, "[recv] aacDecoder_Fillエラー: code=%d\n", derr);
                        continue;
                    }
                    derr = aacDecoder_DecodeFrame(hDec, pcm_out, PCM_OUT_SAMPLES, 0);
                    if (derr == AAC_DEC_NOT_ENOUGH_BITS) continue;
                    if (derr != AAC_DEC_OK) {
                        fprintf(stderr, "[recv] AACデコードエラー: code=%d (継続)\n", derr);
                        continue;
                    }
                    CStreamInfo *si = aacDecoder_GetStreamInfo(hDec);
                    int pcm_bytes = si->frameSize * si->numChannels * (int)sizeof(INT_PCM);
                    ssize_t w = write(STDOUT_FILENO, pcm_out, pcm_bytes); (void)w;
                    total_frames++;
                    total_pcm_bytes += pcm_bytes;
                } else {
                    /* aptX: aptx_decode_sync → S24 → S16 */
                    size_t written = 0, dropped = 0;
                    int synced = 0;
                    size_t processed = aptx_decode_sync(aptx, payload, (size_t)payload_len,
                                                        s24_out, sizeof(s24_out),
                                                        &written, &synced, &dropped);
                    if (!synced && !aptx_syncing) {
                        fprintf(stderr, "[recv] aptX同期喪失。再同期中...\n");
                        aptx_syncing = 1;
                    } else if (synced && aptx_syncing) {
                        fprintf(stderr, "[recv] aptX再同期成功(破棄=%zuバイト)。\n", dropped);
                        aptx_syncing = 0;
                    }
                    if (processed != (size_t)payload_len) {
                        /* 回復不能: 残りは次メッセージで再同期に任せる */
                    }
                    if (written > 0) {
                        int n_samples = (int)(written / 3);
                        s24_to_s16(s24_out, s16_out, n_samples);
                        ssize_t w = write(STDOUT_FILENO, s16_out, (size_t)n_samples * sizeof(int16_t));
                        (void)w;
                        total_frames++;
                        total_pcm_bytes += (long)n_samples * sizeof(int16_t);
                    }
                }
                continue;
            }

            fprintf(stderr, "[recv] 警告: 未知のメッセージ種別 0x%02x を無視。\n", type);
        }

        dropped_before_init_total += dropped_before_init;
        if (hDec) aacDecoder_Close(hDec);
        if (aptx) aptx_finish(aptx);
        srt_close(sock);

        if (!g_should_exit) {
            reconnect_count++;
            usleep(RECONNECT_WAIT_MS * 1000);
        }
    }

    fprintf(stderr, "[recv] 終了。デコードフレーム数=%ld, 合計PCMバイト数=%ld, "
                    "初期化前破棄(累計)=%ld, 再接続回数=%ld\n",
            total_frames, total_pcm_bytes, dropped_before_init_total, reconnect_count);

    srt_cleanup();
    return 0;
}
