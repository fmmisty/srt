/*
 * audio_srt_recv.c
 *
 * SRTソケット経由で音声(生PCM)を受信し、標準出力(stdout)へ書き出す。
 * ALSAの aplay と組み合わせて使う想定:
 *
 *   ./audio_srt_recv 192.168.1.10 9000 | aplay -f S16_LE -r 48000 -c 2 -t raw
 *
 * Step2のプロトタイプ用。エラー処理は最低限。
 */

#include <srt/srt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAYLOAD_SIZE 1316

static void die(const char *msg) {
    fprintf(stderr, "[recv] エラー: %s : %s\n", msg, srt_getlasterror_str());
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "使い方: %s <send_host> <send_port> [latency_ms]\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    int latency_ms = (argc >= 4) ? atoi(argv[3]) : 120;

    srt_startup();

    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) die("srt_create_socket");

    srt_setsockopt(sock, 0, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        fprintf(stderr, "[recv] IPアドレスが不正です: %s\n", host);
        return 1;
    }

    fprintf(stderr, "[recv] %s:%d へ接続中 (latency=%dms)...\n", host, port, latency_ms);

    if (srt_connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR)
        die("srt_connect");

    fprintf(stderr, "[recv] 接続完了。受信を開始します。\n");

    char buf[PAYLOAD_SIZE];
    int n;
    long total_bytes = 0;

    while (1) {
        n = srt_recv(sock, buf, PAYLOAD_SIZE);
        if (n == SRT_ERROR) {
            /* "Connection was broken" は正常終了(送信側close)でも出ることがある。
               無線区間での瞬断はSRT内部のARQで吸収されるため、
               ここに来る場合は完全な切断とみなしてよい。 */
            fprintf(stderr, "[recv] 受信終了: %s\n", srt_getlasterror_str());
            break;
        }
        if (n == 0) continue;

        ssize_t written = write(STDOUT_FILENO, buf, n);
        (void)written; /* プロトタイプにつき戻り値は簡略化 */
        total_bytes += n;
    }

    fprintf(stderr, "[recv] 終了。合計受信バイト数: %ld\n", total_bytes);

    srt_close(sock);
    srt_cleanup();
    return 0;
}
