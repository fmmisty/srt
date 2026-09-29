/*
 * aac_srt_recv.c
 *
 * SRTソケット経由でAAC-LC(raw, TT_MP4_RAW)フレームを受信し、
 * fdk-aacでデコードして生PCM音声(S16LE)を標準出力へ書き出す。
 *
 * 使い方:
 *   ./aac_srt_recv <send_host> <send_port> [latency_ms] | aplay -f S16_LE -r 48000 -c 2 -t raw
 *
 * メッセージ形式: 先頭1byteが種別タグ、以降がペイロード。
 *   0x01 = CONFIG (ASC / AudioSpecificConfig)
 *   0x02 = AUDIO  (AACフレーム本体)
 * 「最初に来たメッセージ=設定」という順序への決め打ちをやめ、種別タグで
 * 明示的に判別する。送信側はCONFIGを約100ms間隔で再送してくるため、
 * デコーダ初期化前に受信したAUDIOは破棄し、次のCONFIGを待てば自己修復する。
 *
 * 接続が切れた場合、本プログラムは終了せず、自動的に再接続を試み続ける
 * (放送用途で常駐稼働させることを想定)。Ctrl+C/SIGTERM等で終了する。
 */

#include <srt/srt.h>
#include <fdk-aac/aacdecoder_lib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define MSG_TYPE_CONFIG 0x01
#define MSG_TYPE_AUDIO  0x02

#define MAX_MSG_SIZE 4096
#define PCM_OUT_SAMPLES 8192 /* デコード出力バッファ(サンプル数、十分大きめに確保) */
#define MAX_CONFIG_SIZE 64
#define RECONNECT_WAIT_MS 1000

static volatile sig_atomic_t g_should_exit = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_should_exit = 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "使い方: %s <send_host> <send_port> [latency_ms=150] [passphrase]\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    int latency_ms = (argc >= 4) ? atoi(argv[3]) : 150;
    const char *passphrase = (argc >= 5) ? argv[4] : NULL;

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

    UCHAR msg_buf[MAX_MSG_SIZE];
    INT_PCM pcm_out[PCM_OUT_SAMPLES];
    long total_frames = 0, total_pcm_bytes = 0, dropped_before_init_total = 0;
    long reconnect_count = 0;

    /* ---- 再接続ループ: 接続が切れても終了せず、自動的に接続をやり直す ---- */
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
                srt_close(sock);
                srt_cleanup();
                return 1;
            }
            srt_setsockopt(sock, 0, SRTO_PASSPHRASE, passphrase, (int)plen);
            if (reconnect_count == 0) fprintf(stderr, "[recv] 暗号化: 有効\n");
        } else {
            if (reconnect_count == 0) fprintf(stderr, "[recv] 暗号化: 無効(平文で受信します)\n");
        }

        if (reconnect_count == 0) {
            fprintf(stderr, "[recv] %s:%d へ接続中 (latency=%dms)...\n", host, port, latency_ms);
        } else {
            fprintf(stderr, "[recv] %s:%d へ再接続中... (%ld回目)\n", host, port, reconnect_count);
        }

        if (srt_connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
            fprintf(stderr, "[recv] srt_connectエラー: %s (%dms後に再試行)\n",
                    srt_getlasterror_str(), RECONNECT_WAIT_MS);
            srt_close(sock);
            reconnect_count++;
            usleep(RECONNECT_WAIT_MS * 1000);
            continue;
        }
        fprintf(stderr, "[recv] 接続完了。CONFIG(ASC)を待っています...\n");

        /* ---- AACデコーダは、CONFIGを受信してから初めて開く(接続ごとに作り直す) ---- */
        HANDLE_AACDECODER hDec = NULL;
        AAC_DECODER_ERROR derr;
        long dropped_before_init = 0;

        /* ---- 受信 & デコードループ(1接続分) ---- */
        while (!g_should_exit) {
            int n = srt_recv(sock, (char *)msg_buf, MAX_MSG_SIZE);
            if (n == SRT_ERROR) {
                fprintf(stderr, "[recv] 接続切断: %s (再接続します)\n", srt_getlasterror_str());
                break; /* 内側ループを抜けて再接続へ */
            }
            if (n <= 1) continue; /* 種別タグ1byte未満は不正メッセージとして無視 */

            UCHAR type = msg_buf[0];
            UCHAR *payload = msg_buf + 1;
            int payload_len = n - 1;

            if (type == MSG_TYPE_CONFIG) {
                if (hDec != NULL) {
                    continue; /* 既に初期化済み。定期再送分は無視 */
                }
                if (payload_len <= 0 || payload_len > MAX_CONFIG_SIZE) {
                    fprintf(stderr, "[recv] 警告: CONFIGのサイズが想定外(%d bytes)。無視します。\n",
                            payload_len);
                    continue;
                }

                hDec = aacDecoder_Open(TT_MP4_RAW, 1);
                if (!hDec) {
                    fprintf(stderr, "[recv] aacDecoder_Openに失敗しました。再接続します。\n");
                    break;
                }
                UCHAR *conf_ptrs[1] = { payload };
                UINT conf_lens[1] = { (UINT)payload_len };
                derr = aacDecoder_ConfigRaw(hDec, conf_ptrs, conf_lens);
                if (derr != AAC_DEC_OK) {
                    fprintf(stderr, "[recv] aacDecoder_ConfigRawに失敗しました: code=%d\n", derr);
                    aacDecoder_Close(hDec);
                    hDec = NULL;
                    continue; /* 次のCONFIG再送を待つ */
                }
                fprintf(stderr, "[recv] AACデコーダ初期化完了(CONFIG %d bytes)。"
                                 "デコードを開始します。破棄した音声フレーム数=%ld\n",
                                 payload_len, dropped_before_init);
                continue;
            }

            if (type == MSG_TYPE_AUDIO) {
                if (hDec == NULL) {
                    dropped_before_init++;
                    continue;
                }

                UCHAR *in_ptrs[1] = { payload };
                UINT in_lens[1] = { (UINT)payload_len };
                UINT bytes_valid = (UINT)payload_len;

                derr = aacDecoder_Fill(hDec, in_ptrs, in_lens, &bytes_valid);
                if (derr != AAC_DEC_OK) {
                    fprintf(stderr, "[recv] aacDecoder_Fillエラー: code=%d\n", derr);
                    continue;
                }

                derr = aacDecoder_DecodeFrame(hDec, pcm_out, PCM_OUT_SAMPLES, 0);
                if (derr == AAC_DEC_NOT_ENOUGH_BITS) {
                    continue;
                }
                if (derr != AAC_DEC_OK) {
                    fprintf(stderr, "[recv] デコードエラー: code=%d (継続)\n", derr);
                    continue;
                }

                CStreamInfo *si = aacDecoder_GetStreamInfo(hDec);
                int pcm_bytes = si->frameSize * si->numChannels * (int)sizeof(INT_PCM);

                ssize_t w = write(STDOUT_FILENO, pcm_out, pcm_bytes);
                (void)w;

                total_frames++;
                total_pcm_bytes += pcm_bytes;
                continue;
            }

            fprintf(stderr, "[recv] 警告: 未知のメッセージ種別 0x%02x を無視しました。\n", type);
        }

        dropped_before_init_total += dropped_before_init;
        if (hDec) aacDecoder_Close(hDec);
        srt_close(sock);

        if (!g_should_exit) {
            reconnect_count++;
            usleep(RECONNECT_WAIT_MS * 1000);
        }
    }

    fprintf(stderr, "[recv] 終了。デコードフレーム数=%ld, 合計PCMバイト数=%ld, "
                     "初期化前に破棄した音声フレーム数(累計)=%ld, 再接続回数=%ld\n",
            total_frames, total_pcm_bytes, dropped_before_init_total, reconnect_count);

    srt_cleanup();
    return 0;
}
