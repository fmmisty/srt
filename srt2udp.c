/*
 * srt2udp.c  ―  SRT→UDP 透過トンネル(受け側)
 *
 * udp2srt が送ってくるSRTストリームを受け、中身を触らず元のUDP/RTPパケットとして
 * 装置(HDIP-3000V等)へ配る。1 SRTメッセージ = 1 UDPデータグラムでパケット境界を保持。
 * 接続が切れても終了せず自動再接続(常駐運用)。
 *
 * 使い方:
 *   ./srt2udp <srt_host> <srt_port> --udp-out=<host:port> [--latency=250] [--passphrase=xxx]
 *     <srt_host:srt_port> : 相手の udp2srt(SRT待受)へ接続
 *     --udp-out           : 装置が受信するUDPアドレス:ポート(ここへ配る)
 *
 * 例:
 *   ./srt2udp 10.0.0.1 9000 --udp-out=192.168.0.30:15000 --latency=250 --passphrase=broadcast-key
 */
#include <srt/srt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define MSG_MAX 1500
#define RECONNECT_WAIT_MS 1000

static volatile sig_atomic_t g_exit = 0;
static void on_sig(int s){ (void)s; g_exit = 1; }

static const char *opt_val(const char *arg, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) == 0 && arg[klen] == '=') return arg + klen + 1;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "使い方: %s <srt_host> <srt_port> --udp-out=<host:port> [--latency=250] [--passphrase=xxx]\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int srt_port = atoi(argv[2]);
    const char *udp_out = NULL; int latency_ms = 250;
    const char *passphrase = NULL;
    for (int i = 3; i < argc; ++i) {
        const char *v;
        if      ((v = opt_val(argv[i], "--udp-out")))    udp_out = v;
        else if ((v = opt_val(argv[i], "--latency")))    latency_ms = atoi(v);
        else if ((v = opt_val(argv[i], "--passphrase"))) passphrase = v;
        else { fprintf(stderr, "[srt2udp] 不明なオプション: %s\n", argv[i]); return 1; }
    }
    if (!udp_out) { fprintf(stderr, "[srt2udp] --udp-out=<host:port> は必須です。\n"); return 1; }

    /* 出力UDP宛先(装置へ) */
    const char *colon = strrchr(udp_out, ':');
    if (!colon) { fprintf(stderr, "[srt2udp] --udp-out は host:port 形式で。\n"); return 1; }
    char dhost[256]; size_t hl = (size_t)(colon - udp_out);
    if (hl >= sizeof(dhost)) hl = sizeof(dhost)-1;
    memcpy(dhost, udp_out, hl); dhost[hl] = 0;
    int dport = atoi(colon + 1);
    int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ufd < 0) { perror("[srt2udp] udp socket"); return 1; }
    struct sockaddr_in dst; memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET; dst.sin_port = htons(dport);
    if (inet_pton(AF_INET, dhost, &dst.sin_addr) != 1) { fprintf(stderr, "[srt2udp] IP不正: %s\n", dhost); return 1; }

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    srt_startup();

    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(srt_port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { fprintf(stderr, "[srt2udp] srt_host不正: %s\n", host); return 1; }

    fprintf(stderr, "[srt2udp] SRT %s:%d → UDP %s:%d (latency=%dms)\n", host, srt_port, dhost, dport, latency_ms);

    unsigned char buf[MSG_MAX];
    long reconn = 0;
    while (!g_exit) {
        SRTSOCKET sock = srt_create_socket();
        int payload = 1456;
        srt_setsockopt(sock, 0, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));
        srt_setsockopt(sock, 0, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
        if (passphrase) {
            size_t pl = strlen(passphrase);
            if (pl < 10 || pl > 79) { fprintf(stderr, "[srt2udp] パスフレーズは10〜79文字。\n"); srt_close(sock); srt_cleanup(); return 1; }
            srt_setsockopt(sock, 0, SRTO_PASSPHRASE, passphrase, (int)pl);
        }
        if (srt_connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
            fprintf(stderr, "[srt2udp] 接続失敗: %s (%dms後再試行)\n", srt_getlasterror_str(), RECONNECT_WAIT_MS);
            srt_close(sock); reconn++; usleep(RECONNECT_WAIT_MS*1000); continue;
        }
        fprintf(stderr, "[srt2udp] 接続完了。中継開始。\n");
        long pkts = 0;
        while (!g_exit) {
            int n = srt_recv(sock, (char *)buf, sizeof(buf));
            if (n == SRT_ERROR) { fprintf(stderr, "[srt2udp] 切断: %s (再接続)\n", srt_getlasterror_str()); break; }
            if (n <= 0) continue;
            sendto(ufd, buf, n, 0, (struct sockaddr *)&dst, sizeof(dst));
            pkts++;
        }
        srt_close(sock);
        if (!g_exit) { reconn++; usleep(RECONNECT_WAIT_MS*1000); }
    }
    srt_cleanup(); return 0;
}
