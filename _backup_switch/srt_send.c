/*
 * srt_send.c  ―  マルチコーデック対応 SRT 音声送信
 *
 * 標準入力(stdin)から生PCM音声(S16LE, インターリーブstereo)を読み込み、
 * 選択したコーデック(AAC-LC / aptX / aptX HD)でエンコードして、SRTソケット
 * 経由で送信する。受信側は CONFIG メッセージに含まれる codec_id を見て、
 * 自動的にデコーダを切り替える(受信側はコーデック指定不要)。
 *
 * 使い方:
 *   arecord -f S16_LE -r 48000 -c 2 -t raw \
 *     | ./srt_send <listen_port> [--codec=aac|aptx|aptxhd] [--samplerate=48000]
 *                  [--channels=2] [--bitrate=96000] [--latency=150] [--passphrase=xxx]
 *
 * 例:
 *   ./srt_send 9000 --codec=aac  --bitrate=128000 --latency=250
 *   ./srt_send 9000 --codec=aptx
 *   ./srt_send 9000 --codec=aptxhd
 *
 * ---- 音声読み取りスレッドについて ----
 * stdin(arecord)の読み取りは専用スレッドで行い、ネットワークの状態とは完全に
 * 切り離す。接続断で srt_accept() ブロック中でも stdin を読み続け、送れない間の
 * 音声は破棄することで、arecord/ALSA 側の overrun を防ぐ(ベース版と同じ設計)。
 */

#include <srt/srt.h>
#include <fdk-aac/aacenc_lib.h>
#include <freeaptx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "srt_codec.h"

/* ---- 読み取りスレッドとメインで共有する単一フレームバッファ ----
   常に「最新の1フレーム」だけを保持し、未消費なら遠慮なく上書き(=古いフレーム破棄)。 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    unsigned char  *buf;
    int             frame_bytes;
    int             has_frame;
    int             eof;
    long            frames_read;
    long            frames_dropped;
} shared_frame_t;

static shared_frame_t g_shared;

static void *reader_thread_func(void *arg) {
    int frame_bytes = *(int *)arg;
    unsigned char *tmp = malloc(frame_bytes);

    while (1) {
        int got = 0;
        while (got < frame_bytes) {
            ssize_t r = read(STDIN_FILENO, (char *)tmp + got, frame_bytes - got);
            if (r <= 0) {
                pthread_mutex_lock(&g_shared.lock);
                g_shared.eof = 1;
                pthread_cond_signal(&g_shared.cond);
                pthread_mutex_unlock(&g_shared.lock);
                free(tmp);
                return NULL;
            }
            got += (int)r;
        }
        pthread_mutex_lock(&g_shared.lock);
        if (g_shared.has_frame) g_shared.frames_dropped++;
        memcpy(g_shared.buf, tmp, frame_bytes);
        g_shared.has_frame = 1;
        g_shared.frames_read++;
        pthread_cond_signal(&g_shared.cond);
        pthread_mutex_unlock(&g_shared.lock);
    }
}

static void die_srt(const char *msg) {
    fprintf(stderr, "[send] SRTエラー: %s : %s\n", msg, srt_getlasterror_str());
    exit(1);
}

/* "--key=value" 形式のオプションから value を返す。合致しなければ NULL。 */
static const char *opt_val(const char *arg, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) == 0 && arg[klen] == '=') return arg + klen + 1;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "使い方: %s <listen_port> [--codec=aac|aptx|aptxhd] [--samplerate=48000]\n"
            "        [--channels=2] [--bitrate=96000] [--latency=150] [--passphrase=xxx]\n",
            argv[0]);
        return 1;
    }
    int port        = atoi(argv[1]);
    int codec       = CODEC_AAC;
    int samplerate  = 48000;
    int channels    = 2;
    int bitrate     = 96000;
    int latency_ms  = 150;
    const char *passphrase = NULL;

    for (int i = 2; i < argc; ++i) {
        const char *v;
        if      ((v = opt_val(argv[i], "--codec"))) {
            codec = codec_id_from_name(v);
            if (codec < 0) { fprintf(stderr, "[send] 不正なコーデック: %s\n", v); return 1; }
        }
        else if ((v = opt_val(argv[i], "--samplerate"))) samplerate = atoi(v);
        else if ((v = opt_val(argv[i], "--channels")))   channels   = atoi(v);
        else if ((v = opt_val(argv[i], "--bitrate")))    bitrate    = atoi(v);
        else if ((v = opt_val(argv[i], "--latency")))    latency_ms = atoi(v);
        else if ((v = opt_val(argv[i], "--passphrase"))) passphrase = v;
        else { fprintf(stderr, "[send] 不明なオプション: %s\n", argv[i]); return 1; }
    }

    if ((codec == CODEC_APTX || codec == CODEC_APTXHD) && channels != 2) {
        fprintf(stderr, "[send] エラー: aptX/aptX HD は stereo(2ch)専用です。\n");
        return 1;
    }

    /* ---- エンコーダ初期化 ---- */
    HANDLE_AACENCODER hEnc = NULL;   /* AAC時のみ使用 */
    struct aptx_context *aptx = NULL; /* aptX時のみ使用 */
    AACENC_InfoStruct info;          /* AAC時のみ有効 */
    memset(&info, 0, sizeof(info));

    int pcm_samples_per_frame; /* 1フレームの総サンプル数(全ch合計) */
    int frame_bytes;           /* 1フレームの S16LE バイト数 */

    if (codec == CODEC_AAC) {
        AACENC_ERROR aerr;
        if ((aerr = aacEncOpen(&hEnc, 0, channels)) != AACENC_OK) {
            fprintf(stderr, "[send] aacEncOpen失敗(code=%d)\n", aerr); return 1;
        }
        aacEncoder_SetParam(hEnc, AACENC_AOT, AOT_AAC_LC);
        aacEncoder_SetParam(hEnc, AACENC_SAMPLERATE, samplerate);
        aacEncoder_SetParam(hEnc, AACENC_CHANNELMODE, channels == 2 ? MODE_2 : MODE_1);
        aacEncoder_SetParam(hEnc, AACENC_CHANNELORDER, 1);
        aacEncoder_SetParam(hEnc, AACENC_BITRATE, bitrate);
        aacEncoder_SetParam(hEnc, AACENC_TRANSMUX, TT_MP4_RAW);
        aacEncoder_SetParam(hEnc, AACENC_AFTERBURNER, 1);
        if ((aerr = aacEncEncode(hEnc, NULL, NULL, NULL, NULL)) != AACENC_OK) {
            fprintf(stderr, "[send] aacEncEncode(init)失敗(code=%d)\n", aerr); return 1;
        }
        if ((aerr = aacEncInfo(hEnc, &info)) != AACENC_OK) {
            fprintf(stderr, "[send] aacEncInfo失敗(code=%d)\n", aerr); return 1;
        }
        pcm_samples_per_frame = info.frameLength * channels;
        frame_bytes = pcm_samples_per_frame * (int)sizeof(INT_PCM);
        fprintf(stderr, "[send] AACエンコーダ初期化完了: frameLength=%u, confSize=%u, "
                        "maxOutBufBytes=%u, bitrate=%d\n",
                info.frameLength, info.confSize, info.maxOutBufBytes, bitrate);
    } else {
        int hd = (codec == CODEC_APTXHD) ? 1 : 0;
        aptx = aptx_init(hd);
        if (!aptx) { fprintf(stderr, "[send] aptx_init失敗\n"); return 1; }
        pcm_samples_per_frame = APTX_FRAME_SAMPLES * channels;
        frame_bytes = pcm_samples_per_frame * (int)sizeof(int16_t);
        fprintf(stderr, "[send] %s エンコーダ初期化完了: frame=%dサンプル(stereo), "
                        "圧縮比 約%s\n",
                codec_name(codec), APTX_FRAME_SAMPLES,
                hd ? "2.7:1(576kbps相当@48k)" : "4:1(384kbps相当@48k)");
    }

    /* ---- 共有フレームバッファ + 読み取りスレッド起動 ---- */
    pthread_mutex_init(&g_shared.lock, NULL);
    pthread_cond_init(&g_shared.cond, NULL);
    g_shared.buf = malloc(frame_bytes);
    g_shared.frame_bytes = frame_bytes;
    g_shared.has_frame = 0;
    g_shared.eof = 0;
    g_shared.frames_read = 0;
    g_shared.frames_dropped = 0;

    pthread_t reader_tid;
    pthread_create(&reader_tid, NULL, reader_thread_func, &frame_bytes);
    fprintf(stderr, "[send] 音声読み取りスレッドを起動しました"
                    "(切断中は音声を破棄し、arecordのブロックを防ぎます)。\n");

    /* ---- SRT初期化(listen -> accept) ---- */
    srt_startup();
    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) die_srt("srt_create_socket");

    int yes = 1;
    srt_setsockopt(sock, 0, SRTO_SENDER, &yes, sizeof(yes));
    srt_setsockopt(sock, 0, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));

    if (passphrase != NULL) {
        size_t plen = strlen(passphrase);
        if (plen < 10 || plen > 79) {
            fprintf(stderr, "[send] エラー: パスフレーズは10〜79文字である必要があります"
                            "(現在%zu文字)。\n", plen);
            return 1;
        }
        int pbkeylen = 16;
        srt_setsockopt(sock, 0, SRTO_PASSPHRASE, passphrase, (int)plen);
        srt_setsockopt(sock, 0, SRTO_PBKEYLEN, &pbkeylen, sizeof(pbkeylen));
        fprintf(stderr, "[send] 暗号化: 有効(AES-%d)\n", pbkeylen * 8);
    } else {
        fprintf(stderr, "[send] 暗号化: 無効(平文で送信します)\n");
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (srt_bind(sock, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) die_srt("srt_bind");
    fprintf(stderr, "[send] ポート %d で待受中 (codec=%s, latency=%dms)...\n",
            port, codec_name(codec), latency_ms);
    if (srt_listen(sock, 1) == SRT_ERROR) die_srt("srt_listen");

    /* ---- CONFIG メッセージ組み立て ----
       [tag][codec_id][samplerate LE(4)][channels][コーデック固有] */
    int aac_conf = (codec == CODEC_AAC) ? (int)info.confSize : 0;
    int config_len = 1 + 1 + 4 + 1 + aac_conf;
    unsigned char *config_msg = malloc(config_len);
    config_msg[0] = MSG_TYPE_CONFIG;
    config_msg[1] = (unsigned char)codec;
    put_u32le(config_msg + 2, (uint32_t)samplerate);
    config_msg[6] = (unsigned char)channels;
    if (codec == CODEC_AAC) memcpy(config_msg + 7, info.confBuf, info.confSize);

    /* CONFIG 再送間隔: 約100msごと */
    int frames_per_100ms;
    if (codec == CODEC_AAC) frames_per_100ms = (samplerate / info.frameLength) / 10;
    else                    frames_per_100ms = (samplerate / APTX_FRAME_SAMPLES) / 10;
    if (frames_per_100ms < 1) frames_per_100ms = 1;
    fprintf(stderr, "[send] CONFIG再送間隔: 約%dフレームごと\n", frames_per_100ms);

    /* ---- 作業バッファ ---- */
    int16_t *pcm_buf = malloc(frame_bytes);

    /* 出力バッファ(先頭1byteは MSG_TYPE_AUDIO タグ) */
    int max_out;
    if (codec == CODEC_AAC) max_out = (int)info.maxOutBufBytes;
    else if (codec == CODEC_APTXHD) max_out = (APTX_FRAME_SAMPLES / 4) * 6;
    else                            max_out = (APTX_FRAME_SAMPLES / 4) * 4;
    unsigned char *out_buf = malloc(1 + max_out);
    out_buf[0] = MSG_TYPE_AUDIO;

    /* aptX用 S24 変換バッファ(全chサンプル×3byte) */
    unsigned char *s24_buf = NULL;
    if (codec != CODEC_AAC) s24_buf = malloc(pcm_samples_per_frame * 3);

    long total_frames = 0, total_bytes = 0, reconnect_count = 0;
    int stdin_eof = 0;
    SRTSOCKET client = SRT_INVALID_SOCK;

    /* ---- 再接続ループ ---- */
    while (!stdin_eof) {
        int sa_len = sizeof(sa);
        client = srt_accept(sock, (struct sockaddr *)&sa, &sa_len);
        if (client == SRT_INVALID_SOCK) {
            fprintf(stderr, "[send] srt_acceptエラー: %s (再試行)\n", srt_getlasterror_str());
            usleep(500 * 1000);
            continue;
        }
        if (reconnect_count == 0) {
            fprintf(stderr, "[send] 受信側が接続しました。\n");
        } else {
            fprintf(stderr, "[send] 受信側が再接続しました。(%ld回目、接続待ち中の破棄=%ld)\n",
                    reconnect_count, g_shared.frames_dropped);
        }

        /* aptX は接続ごとに予測器状態をリセット(新しいストリームとして開始) */
        if (aptx) aptx_reset(aptx);

        if (srt_send(client, (char *)config_msg, config_len) == SRT_ERROR) {
            fprintf(stderr, "[send] CONFIG送信エラー: %s (再接続待ちへ)\n", srt_getlasterror_str());
            srt_close(client);
            reconnect_count++;
            continue;
        }
        fprintf(stderr, "[send] CONFIG送信(codec=%s, %dバイト)。\n", codec_name(codec), config_len);

        int frame_counter = 0;

        /* ---- エンコード & 送信ループ(1接続分) ---- */
        while (1) {
            pthread_mutex_lock(&g_shared.lock);
            while (!g_shared.has_frame && !g_shared.eof)
                pthread_cond_wait(&g_shared.cond, &g_shared.lock);
            if (g_shared.eof && !g_shared.has_frame) {
                pthread_mutex_unlock(&g_shared.lock);
                stdin_eof = 1;
                goto conn_done;
            }
            memcpy(pcm_buf, g_shared.buf, frame_bytes);
            g_shared.has_frame = 0;
            pthread_mutex_unlock(&g_shared.lock);

            int out_len = 0; /* out_buf[1..] に書き込んだエンコード済みバイト数 */

            if (codec == CODEC_AAC) {
                AACENC_BufDesc inBufDesc = {0}, outBufDesc = {0};
                AACENC_InArgs inArgs = {0};
                AACENC_OutArgs outArgs = {0};

                void *inBuf = pcm_buf;
                int inBufId = IN_AUDIO_DATA, inBufSize = frame_bytes, inBufElSize = sizeof(INT_PCM);
                inBufDesc.numBufs = 1; inBufDesc.bufs = &inBuf;
                inBufDesc.bufferIdentifiers = &inBufId;
                inBufDesc.bufSizes = &inBufSize; inBufDesc.bufElSizes = &inBufElSize;

                void *outBuf = out_buf + 1;
                int outBufId = OUT_BITSTREAM_DATA, outBufSize = max_out, outBufElSize = sizeof(UCHAR);
                outBufDesc.numBufs = 1; outBufDesc.bufs = &outBuf;
                outBufDesc.bufferIdentifiers = &outBufId;
                outBufDesc.bufSizes = &outBufSize; outBufDesc.bufElSizes = &outBufElSize;

                inArgs.numInSamples = pcm_samples_per_frame;

                AACENC_ERROR aerr = aacEncEncode(hEnc, &inBufDesc, &outBufDesc, &inArgs, &outArgs);
                if (aerr != AACENC_OK) {
                    fprintf(stderr, "[send] AACエンコードエラー: code=%d\n", aerr);
                    continue;
                }
                out_len = outArgs.numOutBytes;
            } else {
                /* aptX: S16 → S24 → aptx_encode */
                s16_to_s24(pcm_buf, s24_buf, pcm_samples_per_frame);
                size_t written = 0;
                size_t processed = aptx_encode(aptx, s24_buf, (size_t)pcm_samples_per_frame * 3,
                                               out_buf + 1, max_out, &written);
                if (processed != (size_t)pcm_samples_per_frame * 3) {
                    fprintf(stderr, "[send] aptXエンコードが途中停止(処理=%zu/%d)\n",
                            processed, pcm_samples_per_frame * 3);
                }
                out_len = (int)written;
            }

            if (out_len > 0) {
                int sent = srt_send(client, (char *)out_buf, out_len + 1);
                if (sent == SRT_ERROR) {
                    fprintf(stderr, "[send] 送信エラー(接続切断とみなす): %s\n", srt_getlasterror_str());
                    break;
                }
                total_frames++;
                total_bytes += sent;

                if (++frame_counter >= frames_per_100ms) {
                    frame_counter = 0;
                    if (srt_send(client, (char *)config_msg, config_len) == SRT_ERROR)
                        fprintf(stderr, "[send] CONFIG再送エラー: %s\n", srt_getlasterror_str());
                }
            }
        }

        usleep((latency_ms + 300) * 1000);
        srt_close(client);
        client = SRT_INVALID_SOCK;
        reconnect_count++;
        fprintf(stderr, "[send] 接続を閉じました。次の接続を待ちます"
                        "(この間、音声は読み取りつつ破棄されます)。\n");
    }

conn_done:
    if (client != SRT_INVALID_SOCK) {
        usleep((latency_ms + 300) * 1000);
        srt_close(client);
    }
    fprintf(stderr, "[send] 送信完了。フレーム数=%ld, 合計バイト数=%ld, 再接続回数=%ld, "
                    "読取フレーム数=%ld, 破棄フレーム数=%ld\n",
            total_frames, total_bytes, reconnect_count,
            g_shared.frames_read, g_shared.frames_dropped);

    pthread_join(reader_tid, NULL);
    pthread_mutex_destroy(&g_shared.lock);
    pthread_cond_destroy(&g_shared.cond);
    free(g_shared.buf);
    free(pcm_buf);
    free(out_buf);
    free(config_msg);
    free(s24_buf);
    srt_close(sock);
    srt_cleanup();
    if (hEnc) aacEncClose(&hEnc);
    if (aptx) aptx_finish(aptx);
    return 0;
}
