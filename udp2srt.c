/*
 * udp2srt.c  ―  UDP→SRT 透過トンネル(送り側)
 *
 * 装置(HDIP-3000V等)が出すUDP/RTPパケットを、中身を一切触らずにSRTで送る。
 * 1 UDPデータグラム = 1 SRTメッセージ として運ぶのでパケット境界(RTP)は保たれる。
 * コーデックは装置側で選択(CLEAR/Opus/SBADPCM/LPCM等)。本プログラムは非関与。
 *
 * 使い方:
 *   ./udp2srt <listen_port> --udp-in=<port> [--latency=250] [--passphrase=xxx]
 *     <listen_port> : SRTの待受ポート(相手の srt2udp がここへ接続)
 *     --udp-in      : 装置がUDPを送ってくるローカルポート(ここで待受)
 *
 * 例: 装置のRTP送信先を localhost:15000 にして
 *   ./udp2srt 9000 --udp-in=15000 --latency=250 --passphrase=broadcast-key
 *
 * 未接続の間に装置から届くUDPはカーネルの受信バッファで自然に溢れる(=破棄)。
 * 接続確立後のみ中継する(放送用途のリアルタイム性を優先)。
 */
#include <srt/srt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define DGRAM_MAX 1500          /* MTU内のUDPデータグラム上限 */
#define SRT_PAYLOAD 1456        /* SRTの最大ペイロード(これ以下のデータグラムを想定) */

static const char *opt_val(const char *arg, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) == 0 && arg[klen] == '=') return arg + klen + 1;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "使い方: %s <listen_port> --udp-in=<port> [--latency=250] [--passphrase=xxx]\n", argv[0]);
        return 1;
    }
    int srt_port = atoi(argv[1]);
    int udp_in = 0, latency_ms = 250;
    const char *passphrase = NULL;
    for (int i = 2; i < argc; ++i) {
        const char *v;
        if      ((v = opt_val(argv[i], "--udp-in")))     udp_in = atoi(v);
        else if ((v = opt_val(argv[i], "--latency")))    latency_ms = atoi(v);
        else if ((v = opt_val(argv[i], "--passphrase"))) passphrase = v;
        else { fprintf(stderr, "[udp2srt] 不明なオプション: %s\n", argv[i]); return 1; }
    }
    if (udp_in <= 0) { fprintf(stderr, "[udp2srt] --udp-in=<port> は必須です。\n"); return 1; }

    /* UDP受信ソケット(装置から) */
    int ufd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ufd < 0) { perror("[udp2srt] udp socket"); return 1; }
    struct sockaddr_in ua; memset(&ua, 0, sizeof(ua));
    ua.sin_family = AF_INET; ua.sin_addr.s_addr = INADDR_ANY; ua.sin_port = htons(udp_in);
    if (bind(ufd, (struct sockaddr *)&ua, sizeof(ua)) < 0) { perror("[udp2srt] udp bind"); return 1; }

    srt_startup();
    SRTSOCKET sock = srt_create_socket();
    int yes = 1, payload = SRT_PAYLOAD;
    srt_setsockopt(sock, 0, SRTO_SENDER, &yes, sizeof(yes));
    srt_setsockopt(sock, 0, SRTO_LATENCY, &latency_ms, sizeof(latency_ms));
    srt_setsockopt(sock, 0, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
    if (passphrase) {
        size_t pl = strlen(passphrase);
        if (pl < 10 || pl > 79) { fprintf(stderr, "[udp2srt] パスフレーズは10〜79文字。\n"); return 1; }
        int kl = 16;
        srt_setsockopt(sock, 0, SRTO_PASSPHRASE, passphrase, (int)pl);
        srt_setsockopt(sock, 0, SRTO_PBKEYLEN, &kl, sizeof(kl));
        fprintf(stderr, "[udp2srt] 暗号化: 有効\n");
    }

    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(srt_port); sa.sin_addr.s_addr = INADDR_ANY;
    if (srt_bind(sock, (struct sockaddr *)&sa, sizeof(sa)) == SRT_ERROR) {
        fprintf(stderr, "[udp2srt] srt_bind: %s\n", srt_getlasterror_str()); return 1;
    }
    if (srt_listen(sock, 1) == SRT_ERROR) {
        fprintf(stderr, "[udp2srt] srt_listen: %s\n", srt_getlasterror_str()); return 1;
    }
    fprintf(stderr, "[udp2srt] UDP :%d → SRT :%d で待受 (latency=%dms)\n", udp_in, srt_port, latency_ms);

    unsigned char buf[DGRAM_MAX];
    long conns = 0;
    while (1) {
        int alen = sizeof(sa);
        SRTSOCKET cli = srt_accept(sock, (struct sockaddr *)&sa, &alen);
        if (cli == SRT_INVALID_SOCK) { usleep(500*1000); continue; }
        fprintf(stderr, "[udp2srt] 受信側が接続(%ld)。中継開始。\n", ++conns);
        long pkts = 0, bytes = 0;
        while (1) {
            ssize_t r = recv(ufd, buf, sizeof(buf), 0);
            if (r <= 0) continue;
            if (srt_send(cli, (char *)buf, (int)r) == SRT_ERROR) {
                fprintf(stderr, "[udp2srt] SRT送信エラー(切断とみなす): %s。中継=%ldpkt/%ldB\n",
                        srt_getlasterror_str(), pkts, bytes);
                break;
            }
            pkts++; bytes += r;
        }
        srt_close(cli);
        fprintf(stderr, "[udp2srt] 接続を閉じました。次の接続を待ちます。\n");
    }
    srt_close(sock); srt_cleanup(); return 0;
}
