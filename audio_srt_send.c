/*
 * audio_srt_send.c
 *
 * 標準入力(stdin)から生のPCM音声データを読み込み、SRTソケット経由で送信する。
 * ALSAの arecord と組み合わせて使う想定:
 *
 *   arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -t raw | ./audio_srt_send 9000
 *
 * Step2のプロトタイプ用。エラー処理は最低限。
 */

#include <srt/srt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAYLOAD_SIZE 1316  /* SRTのライブモード推奨ペイロードサイズ */

static void die(const char *msg) {
    fprintf(stderr, "[send] エラー: %s : %s\n", msg, srt_getlasterror_str());
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "使い方: %s <listen_port> [latency_ms]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    int latency_ms = (argc >= 3) ? atoi(argv[2]) : 120; /* 無線区間向けに少し余裕を持たせる */

    srt_startup();

    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) die("srt_create_socket");

    int yes = 1;
    srt_setsockopt(sock, 0, SRTO_SENDER, &yes, sizeof(yes));
    srt_setsockopt(sock, 0, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (srt_bind(sock, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR)
        die("srt_bind");

    fprintf(stderr, "[send] ポート %d で待受中 (latency=%dms)...\n", port, latency_ms);

    if (srt_listen(sock, 1) == SRT_ERROR) die("srt_listen");

    int accept_sa_len = sizeof(sa);
    SRTSOCKET client = srt_accept(sock, (struct sockaddr *)&sa, &accept_sa_len);
    if (client == SRT_INVALID_SOCK) die("srt_accept");

    fprintf(stderr, "[send] 受信側が接続しました。送信を開始します。\n");

    char buf[PAYLOAD_SIZE];
    ssize_t n;
    long total_bytes = 0;

    while ((n = read(STDIN_FILENO, buf, PAYLOAD_SIZE)) > 0) {
        int sent = srt_send(client, buf, (int)n);
        if (sent == SRT_ERROR) {
            fprintf(stderr, "[send] 送信エラー: %s\n", srt_getlasterror_str());
            break;
        }
        total_bytes += sent;
    }

    fprintf(stderr, "[send] 送信完了。合計送信バイト数: %ld\n", total_bytes);

    /* TSBPD(遅延配信)により受信側はlatency_ms分のデータをまだ再生待ちしている。
       即座にcloseすると未配信分が破棄されるため、猶予を置いてからcloseする。
       (実運用ではプロセスを継続稼働させ続けるためこの問題は起きないが、
        本プロトタイプのように単発転送で終了する場合は必須) */
    usleep((latency_ms + 300) * 1000);

    srt_close(client);
    srt_close(sock);
    srt_cleanup();
    return 0;
}
