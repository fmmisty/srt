/*
 * aac_srt_send.c
 *
 * 標準入力(stdin)から生PCM音声(S16LE, インターリーブ)を読み込み、
 * fdk-aacでAAC-LCエンコードし、SRTソケット経由で送信する。
 *
 * 使い方:
 *   arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -t raw \
 *     | ./aac_srt_send <listen_port> [samplerate] [channels] [bitrate] [latency_ms] [passphrase]
 *
 * 例(FMステレオ, 48kHz, 96kbps):
 *   ./aac_srt_send 9000 48000 2 96000 150
 *
 * ---- 音声読み取りスレッドについて ----
 * stdin(arecord)の読み取りは専用スレッドで行い、ネットワークの状態とは
 * 完全に切り離している。理由: Linuxのパイプバッファは既定64KB程度しかなく、
 * 48kHz/16bit/stereoの音声では約0.3秒分にしかならない。受信側との接続が
 * 切れて再接続待ち(srt_accept()でブロック)している間、メインスレッドが
 * stdinを読めなくなると、0.3秒程度でarecord側のwrite()がブロックし、
 * 実機ではALSAの録音バッファもすぐに溢れて音声データが失われる
 * (「overrun」)。読み取りスレッドを分離することで、接続状態に関わらず
 * 常にstdinを読み続け、送信できない間の音声は「捨てる」設計にしている
 * (数十秒以上の長時間断線や電源断からの復旧までは考慮しない)。
 */

#include <srt/srt.h>
#include <fdk-aac/aacenc_lib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define MSG_TYPE_CONFIG 0x01
#define MSG_TYPE_AUDIO  0x02

/* ---- 読み取りスレッドとメインスレッドで共有する単一フレームバッファ ----
   常に「最新の1フレーム」だけを保持する。メインスレッドが接続待ちで
   消費できていない間は、読み取りスレッドが遠慮なく上書きし続ける
   (=古いフレームは捨てる)。 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    unsigned char  *buf;      /* pcm_frame_bytes 分 */
    int             frame_bytes;
    int             has_frame; /* 未消費の新しいフレームがあるか */
    int             eof;       /* stdinがEOFに達したか */
    long            frames_read;
    long            frames_dropped; /* メインが消費する前に上書きされた数 */
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
        if (g_shared.has_frame) {
            g_shared.frames_dropped++; /* 前のフレームがまだ未消費 = 送れていない期間 */
        }
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

static void die_aac(const char *msg, AACENC_ERROR err) {
    fprintf(stderr, "[send] AACエンコーダエラー: %s (code=%d)\n", msg, err);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "使い方: %s <listen_port> [samplerate=48000] [channels=2] "
                         "[bitrate=96000] [latency_ms=150] [passphrase]\n", argv[0]);
        return 1;
    }
    int port        = atoi(argv[1]);
    int samplerate  = (argc >= 3) ? atoi(argv[2]) : 48000;
    int channels    = (argc >= 4) ? atoi(argv[3]) : 2;
    int bitrate     = (argc >= 5) ? atoi(argv[4]) : 96000;
    int latency_ms  = (argc >= 6) ? atoi(argv[5]) : 150;
    const char *passphrase = (argc >= 7) ? argv[6] : NULL;

    /* ---- AACエンコーダ初期化 ---- */
    HANDLE_AACENCODER hEnc;
    AACENC_ERROR aerr;

    if ((aerr = aacEncOpen(&hEnc, 0, channels)) != AACENC_OK)
        die_aac("aacEncOpen", aerr);

    aacEncoder_SetParam(hEnc, AACENC_AOT, AOT_AAC_LC);
    aacEncoder_SetParam(hEnc, AACENC_SAMPLERATE, samplerate);
    aacEncoder_SetParam(hEnc, AACENC_CHANNELMODE, channels == 2 ? MODE_2 : MODE_1);
    aacEncoder_SetParam(hEnc, AACENC_CHANNELORDER, 1);
    aacEncoder_SetParam(hEnc, AACENC_BITRATE, bitrate);
    aacEncoder_SetParam(hEnc, AACENC_TRANSMUX, TT_MP4_RAW);
    aacEncoder_SetParam(hEnc, AACENC_AFTERBURNER, 1);

    if ((aerr = aacEncEncode(hEnc, NULL, NULL, NULL, NULL)) != AACENC_OK)
        die_aac("aacEncEncode(init)", aerr);

    AACENC_InfoStruct info;
    if ((aerr = aacEncInfo(hEnc, &info)) != AACENC_OK)
        die_aac("aacEncInfo", aerr);

    fprintf(stderr, "[send] AACエンコーダ初期化完了: frameLength=%u, confSize=%u, "
                     "maxOutBufBytes=%u\n", info.frameLength, info.confSize, info.maxOutBufBytes);

    /* ---- 共有フレームバッファ + 読み取りスレッド起動 ---- */
    int pcm_samples_per_frame = info.frameLength * channels;
    int frame_bytes = pcm_samples_per_frame * (int)sizeof(INT_PCM);

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
    fprintf(stderr, "[send] ポート %d で待受中 (latency=%dms)...\n", port, latency_ms);
    if (srt_listen(sock, 1) == SRT_ERROR) die_srt("srt_listen");

    UCHAR *config_msg = malloc(1 + info.confSize);
    config_msg[0] = MSG_TYPE_CONFIG;
    memcpy(config_msg + 1, info.confBuf, info.confSize);

    int config_resend_interval = (samplerate / info.frameLength) / 10;
    if (config_resend_interval < 1) config_resend_interval = 1;
    fprintf(stderr, "[send] CONFIG再送間隔: 約%d フレームごと(約%dms)\n",
            config_resend_interval,
            (int)(1000.0 * config_resend_interval * info.frameLength / samplerate));

    INT_PCM *pcm_buf = malloc(frame_bytes);
    UCHAR *out_buf = malloc(1 + info.maxOutBufBytes);
    out_buf[0] = MSG_TYPE_AUDIO;

    long total_frames = 0, total_bytes = 0;
    long reconnect_count = 0;
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
            fprintf(stderr, "[send] 受信側が再接続しました。(%ld回目の再接続、"
                             "接続待ち中に破棄したフレーム数=%ld)\n",
                    reconnect_count, g_shared.frames_dropped);
        }

        if (srt_send(client, (char *)config_msg, (int)(1 + info.confSize)) == SRT_ERROR) {
            fprintf(stderr, "[send] CONFIG送信エラー: %s (再接続待ちへ)\n", srt_getlasterror_str());
            srt_close(client);
            reconnect_count++;
            continue;
        }
        fprintf(stderr, "[send] AAC設定(ASC, %u bytes)を送信しました。\n", info.confSize);

        int frame_counter = 0;

        /* ---- エンコード & 送信ループ(1接続分) ----
           共有バッファから「最新フレーム」を取り出して処理する。
           読み取りスレッドは並行して独立に進み続ける。 */
        while (1) {
            pthread_mutex_lock(&g_shared.lock);
            while (!g_shared.has_frame && !g_shared.eof) {
                pthread_cond_wait(&g_shared.cond, &g_shared.lock);
            }
            if (g_shared.eof && !g_shared.has_frame) {
                pthread_mutex_unlock(&g_shared.lock);
                stdin_eof = 1;
                goto conn_done;
            }
            memcpy(pcm_buf, g_shared.buf, frame_bytes);
            g_shared.has_frame = 0;
            pthread_mutex_unlock(&g_shared.lock);

            AACENC_BufDesc inBufDesc = {0}, outBufDesc = {0};
            AACENC_InArgs inArgs = {0};
            AACENC_OutArgs outArgs = {0};

            void *inBuf = pcm_buf;
            int inBufId = IN_AUDIO_DATA;
            int inBufSize = frame_bytes;
            int inBufElSize = sizeof(INT_PCM);
            inBufDesc.numBufs = 1;
            inBufDesc.bufs = &inBuf;
            inBufDesc.bufferIdentifiers = &inBufId;
            inBufDesc.bufSizes = &inBufSize;
            inBufDesc.bufElSizes = &inBufElSize;

            void *outBuf = out_buf + 1;
            int outBufId = OUT_BITSTREAM_DATA;
            int outBufSize = (int)info.maxOutBufBytes;
            int outBufElSize = sizeof(UCHAR);
            outBufDesc.numBufs = 1;
            outBufDesc.bufs = &outBuf;
            outBufDesc.bufferIdentifiers = &outBufId;
            outBufDesc.bufSizes = &outBufSize;
            outBufDesc.bufElSizes = &outBufElSize;

            inArgs.numInSamples = pcm_samples_per_frame;

            aerr = aacEncEncode(hEnc, &inBufDesc, &outBufDesc, &inArgs, &outArgs);
            if (aerr != AACENC_OK) {
                fprintf(stderr, "[send] エンコードエラー: code=%d\n", aerr);
                continue;
            }

            if (outArgs.numOutBytes > 0) {
                int sent = srt_send(client, (char *)out_buf, outArgs.numOutBytes + 1);
                if (sent == SRT_ERROR) {
                    fprintf(stderr, "[send] 送信エラー(接続切断とみなす): %s\n",
                            srt_getlasterror_str());
                    break;
                }
                total_frames++;
                total_bytes += sent;

                frame_counter++;
                if (frame_counter >= config_resend_interval) {
                    frame_counter = 0;
                    if (srt_send(client, (char *)config_msg, (int)(1 + info.confSize)) == SRT_ERROR) {
                        fprintf(stderr, "[send] CONFIG再送エラー: %s\n", srt_getlasterror_str());
                    }
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
                     "読取スレッドが読んだフレーム数=%ld, 未接続中に破棄したフレーム数=%ld\n",
            total_frames, total_bytes, reconnect_count,
            g_shared.frames_read, g_shared.frames_dropped);

    pthread_join(reader_tid, NULL);
    pthread_mutex_destroy(&g_shared.lock);
    pthread_cond_destroy(&g_shared.cond);
    free(g_shared.buf);
    free(pcm_buf);
    free(out_buf);
    free(config_msg);
    srt_close(sock);
    srt_cleanup();
    aacEncClose(&hEnc);
    return 0;
}
