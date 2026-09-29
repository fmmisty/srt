/*
 * srt_recv.c  ―  マルチコーデック対応 SRT 音声受信(コーデック非依存本体)
 *
 * SRTで受信した音声を、CONFIG の codec_id を見て自動選択したコーデックで
 * デコードし、生PCM(S16LE stereo)を標準出力へ書き出す。デコードは
 * srt_codec_api.h の codec_t 越しに呼ぶので本体はコーデックの中身を知らない。
 * 受信側はコーデック指定不要(送信側の設定に自動追従)。
 *
 * 使い方:
 *   ./srt_recv <send_host> <send_port> [--latency=150] [--passphrase=xxx] \
 *     | aplay -f S16_LE -r 48000 -c 2 -t raw
 *
 * CONFIG は約100ms毎に再送されるので、デコーダ初期化前に届いた AUDIO は破棄し、
 * 次の CONFIG を待てば自己修復する。接続断でも終了せず自動再接続(常駐稼働想定)。
 */
#include <srt/srt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "srt_codec_api.h"

#define PCM_OUT_CAP       65536   /* デコード出力(S16LE)の上限バイト数 */
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
        fprintf(stderr, "使い方: %s <send_host> <send_port> [--latency=150] [--passphrase=xxx]\n"
                        "        [--udp-out=<host:port>]  ← 省略時はstdout(aplay)、指定時はそのUDPへPCM送出\n",
                        argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    int latency_ms = 150;
    const char *passphrase = NULL;
    const char *udp_out = NULL;   /* 指定時は stdout ではなく UDP へPCMを配る */

    for (int i = 3; i < argc; ++i) {
        const char *v;
        if      ((v = opt_val(argv[i], "--latency")))    latency_ms = atoi(v);
        else if ((v = opt_val(argv[i], "--passphrase"))) passphrase = v;
        else if ((v = opt_val(argv[i], "--udp-out")))    udp_out = v;
        else { fprintf(stderr, "[recv] 不明なオプション: %s\n", argv[i]); return 1; }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    srt_startup();

    /* 音声出力先の決定: stdout か UDP。 */
    int udp_fd = -1;
    struct sockaddr_in udp_dest;
    if (udp_out) {
        const char *colon = strrchr(udp_out, ':');
        if (!colon) { fprintf(stderr, "[recv] --udp-out は host:port 形式で。\n"); return 1; }
        char dhost[256]; size_t hl = (size_t)(colon - udp_out);
        if (hl >= sizeof(dhost)) hl = sizeof(dhost) - 1;
        memcpy(dhost, udp_out, hl); dhost[hl] = 0;
        int dport = atoi(colon + 1);
        udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_fd < 0) { perror("[recv] udp socket"); return 1; }
        memset(&udp_dest, 0, sizeof(udp_dest));
        udp_dest.sin_family = AF_INET; udp_dest.sin_port = htons(dport);
        if (inet_pton(AF_INET, dhost, &udp_dest.sin_addr) != 1) {
            fprintf(stderr, "[recv] --udp-out のIPが不正: %s\n", dhost); return 1;
        }
        fprintf(stderr, "[recv] 音声出力: UDP %s:%d へ送出 (受信機器をここで受ける)\n", dhost, dport);
    } else {
        fprintf(stderr, "[recv] 音声出力: stdout\n");
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "[recv] IPアドレスが不正です: %s\n", host);
        return 1;
    }

    unsigned char msg_buf[MAX_MSG_SIZE];
    unsigned char *pcm_out = malloc(PCM_OUT_CAP);
    long total_frames = 0, total_pcm_bytes = 0, dropped_before_init_total = 0, reconnect_count = 0;

    while (!g_should_exit) {
        SRTSOCKET sock = srt_create_socket();
        if (sock == SRT_INVALID_SOCK) {
            fprintf(stderr, "[recv] srt_create_socketエラー: %s\n", srt_getlasterror_str());
            usleep(RECONNECT_WAIT_MS * 1000); continue;
        }
        srt_setsockopt(sock, 0, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));
        if (passphrase != NULL) {
            size_t plen = strlen(passphrase);
            if (plen < 10 || plen > 79) {
                fprintf(stderr, "[recv] エラー: パスフレーズは10〜79文字(現在%zu)。\n", plen);
                srt_close(sock); srt_cleanup(); return 1;
            }
            srt_setsockopt(sock, 0, SRTO_PASSPHRASE, passphrase, (int)plen);
            if (reconnect_count == 0) fprintf(stderr, "[recv] 暗号化: 有効\n");
        } else if (reconnect_count == 0) {
            fprintf(stderr, "[recv] 暗号化: 無効(平文で受信)\n");
        }

        if (reconnect_count == 0)
            fprintf(stderr, "[recv] %s:%d へ接続中 (latency=%dms)...\n", host, port, latency_ms);
        else
            fprintf(stderr, "[recv] %s:%d へ再接続中... (%ld回目)\n", host, port, reconnect_count);

        if (srt_connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
            fprintf(stderr, "[recv] srt_connectエラー: %s (%dms後に再試行)\n",
                    srt_getlasterror_str(), RECONNECT_WAIT_MS);
            srt_close(sock); reconnect_count++;
            usleep(RECONNECT_WAIT_MS * 1000); continue;
        }
        fprintf(stderr, "[recv] 接続完了。CONFIGを待っています...\n");

        const codec_t *codec = NULL;
        void *dec = NULL;
        int  cur_cid = -1;   /* 現在のcodec_id。CONFIGで変わったら無停止で切替 */
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

            /* ===== CONFIG ===== */
            if (type == MSG_TYPE_CONFIG) {
                if (payload_len < 6) {
                    fprintf(stderr, "[recv] 警告: CONFIGが短すぎ(%d)。無視。\n", payload_len);
                    continue;
                }
                int cid       = payload[0];
                uint32_t sr   = get_u32le(payload + 1);
                int ch        = payload[5];
                const unsigned char *cfg = payload + 6;
                int cfg_len   = payload_len - 6;

                if (dec != NULL && cid == cur_cid) continue;  /* 同一コーデックの定期再送は無視 */

                const codec_t *c = codec_get_by_id(cid);
                if (!c) { fprintf(stderr, "[recv] 警告: 未知のcodec_id=%d。無視。\n", cid); continue; }

                void *nd = c->dec_open((int)sr, ch, cfg, cfg_len);
                if (!nd) {
                    fprintf(stderr, "[recv] コーデック '%s' のデコーダ初期化失敗。次のCONFIGを待つ。\n", c->name);
                    continue;
                }
                int was_switch = (dec != NULL);
                if (dec && codec) codec->dec_close(dec);      /* 切替時は旧デコーダを閉じる */
                dec = nd; codec = c; cur_cid = cid;
                fprintf(stderr, "[recv] デコーダ%s: codec=%s sr=%u ch=%d\n",
                        was_switch ? "切替" : "初期化", codec->name, sr, ch);
                continue;
            }

            /* ===== AUDIO ===== */
            if (type == MSG_TYPE_AUDIO) {
                if (dec == NULL) { dropped_before_init++; continue; }
                int pcm_bytes = codec->dec_decode(dec, payload, payload_len,
                                                  (int16_t *)pcm_out, PCM_OUT_CAP);
                if (pcm_bytes < 0) { fprintf(stderr, "[recv] デコードエラー(継続)。\n"); continue; }
                if (pcm_bytes == 0) continue;
                if (udp_fd >= 0) {
                    sendto(udp_fd, pcm_out, pcm_bytes, 0,
                           (struct sockaddr *)&udp_dest, sizeof(udp_dest));
                } else {
                    ssize_t w = write(STDOUT_FILENO, pcm_out, pcm_bytes); (void)w;
                }
                total_frames++; total_pcm_bytes += pcm_bytes;
                continue;
            }

            fprintf(stderr, "[recv] 警告: 未知のメッセージ種別 0x%02x を無視。\n", type);
        }

        dropped_before_init_total += dropped_before_init;
        if (dec && codec) codec->dec_close(dec);
        srt_close(sock);
        if (!g_should_exit) { reconnect_count++; usleep(RECONNECT_WAIT_MS * 1000); }
    }

    fprintf(stderr, "[recv] 終了。frames=%ld pcm_bytes=%ld dropped_before_init=%ld reconnect=%ld\n",
            total_frames, total_pcm_bytes, dropped_before_init_total, reconnect_count);
    free(pcm_out);
    srt_cleanup();
    return 0;
}
