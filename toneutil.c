/*
 * toneutil.c  ―  スモークテスト用の簡易ツール(外部依存なし)
 *   toneutil gen <秒> [--paced]        : 48kHz S16LE stereo のトーン(L440/R660)をstdoutへ
 *   toneutil check                     : stdinのS16LE stereoのRMS/推定周波数を表示
 *   toneutil udpsend <host> <port> <秒>: トーンをUDPで送る(装置シミュレータ, 実時間ペース)
 *   toneutil udprecv <port>            : UDPで受けたPCMをstdoutへ(1秒無通信で終了)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>

#define SR 48000

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: toneutil gen <sec> [--paced] | check | udpsend <host> <port> <sec> | udprecv <port>\n"); return 1; }

    /* ===== UDP送信(装置シミュレータ): 実時間ペースで1024Bデータグラムを送る ===== */
    if (strcmp(argv[1], "udpsend") == 0) {
        if (argc < 5) { fprintf(stderr, "usage: toneutil udpsend <host> <port> <sec>\n"); return 1; }
        const char *host = argv[2]; int port = atoi(argv[3]); double sec = atof(argv[4]);
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in da; memset(&da,0,sizeof da);
        da.sin_family=AF_INET; da.sin_port=htons(port);
        if (inet_pton(AF_INET, host, &da.sin_addr)!=1){ fprintf(stderr,"bad host\n"); return 1; }
        long n = (long)(sec * SR);
        int16_t buf[512]; int fill = 0;   /* 512 int16 = 256 stereo = 1024B/データグラム */
        for (long i = 0; i < n; ++i) {
            double t = (double)i / SR;
            buf[fill++] = (int16_t)(20000.0 * sin(2*M_PI*440.0*t));
            buf[fill++] = (int16_t)(20000.0 * sin(2*M_PI*660.0*t));
            if (fill == 512) {
                sendto(fd, buf, sizeof(buf), 0, (struct sockaddr*)&da, sizeof da);
                fill = 0;
                if ((i % 4800) >= 4799 - 1) { } /* noop */
                usleep(256*1000000/SR);        /* 256サンプル分=約5.33ms 実時間ペース */
            }
        }
        return 0;
    }

    /* ===== UDPテキスト送信: stdinの内容を1データグラムで送る(呼制御テスト用) ===== */
    if (strcmp(argv[1], "udptext") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: toneutil udptext <host> <port>  (本文はstdin)\n"); return 1; }
        const char *host = argv[2]; int port = atoi(argv[3]);
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in da; memset(&da,0,sizeof da);
        da.sin_family=AF_INET; da.sin_port=htons(port);
        if (inet_pton(AF_INET, host, &da.sin_addr)!=1){ fprintf(stderr,"bad host\n"); return 1; }
        unsigned char buf[4096]; int n=(int)fread(buf,1,sizeof buf,stdin);
        if (n<=0){ fprintf(stderr,"empty\n"); return 1; }
        sendto(fd, buf, n, 0, (struct sockaddr*)&da, sizeof da);
        return 0;
    }

    /* ===== UDP受信: 受けたPCMをstdoutへ。1秒無通信で終了 ===== */
    if (strcmp(argv[1], "udprecv") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: toneutil udprecv <port>\n"); return 1; }
        int port = atoi(argv[2]);
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a; memset(&a,0,sizeof a);
        a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(port);
        if (argc >= 4) inet_pton(AF_INET, argv[3], &a.sin_addr);  /* 任意のbind IP(loopback別名テスト用) */
        if (bind(fd,(struct sockaddr*)&a,sizeof a)<0){ perror("bind"); return 1; }
        struct timeval tv; tv.tv_sec=1; tv.tv_usec=0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        unsigned char dg[65536]; int got_any=0;
        while (1) {
            ssize_t r = recv(fd, dg, sizeof dg, 0);
            if (r <= 0) { if (got_any) break; else continue; } /* 開始待ちは継続、開始後の無通信で終了 */
            got_any = 1;
            if (fwrite(dg,1,r,stdout)!=(size_t)r) break;
        }
        return 0;
    }

    if (strcmp(argv[1], "gen") == 0) {
        double sec = (argc >= 3) ? atof(argv[2]) : 3.0;
        long n = (long)(sec * SR);
        int paced = (argc >= 4 && strcmp(argv[3], "--paced") == 0);
        for (long i = 0; i < n; ++i) {
            double t = (double)i / SR;
            int16_t l = (int16_t)(20000.0 * sin(2*M_PI*440.0*t));
            int16_t r = (int16_t)(20000.0 * sin(2*M_PI*660.0*t));
            int16_t s[2] = { l, r };
            if (fwrite(s, sizeof(int16_t), 2, stdout) != 2) return 1;
            /* --paced: 0.1秒(4800サンプル)ごとに実時間へ同期し、送信側の
               「最新フレームのみ保持」設計でのフレーム破棄を防ぐ。 */
            if (paced && (i % 4800) == 4799) { fflush(stdout); usleep(100000); }
        }
        return 0;
    }

    if (strcmp(argv[1], "check") == 0) {
        long nframes = 0;
        double sumL = 0, sumR = 0;
        long zcL = 0, zcR = 0;
        int16_t prevL = 0, prevR = 0, first = 1;
        int16_t s[2];
        while (fread(s, sizeof(int16_t), 2, stdin) == 2) {
            double l = s[0], r = s[1];
            sumL += l*l; sumR += r*r;
            if (!first) {
                if ((prevL < 0 && s[0] >= 0) || (prevL >= 0 && s[0] < 0)) zcL++;
                if ((prevR < 0 && s[1] >= 0) || (prevR >= 0 && s[1] < 0)) zcR++;
            }
            prevL = s[0]; prevR = s[1]; first = 0;
            nframes++;
        }
        if (nframes == 0) { fprintf(stderr, "check: 入力が空です\n"); return 2; }
        double dur = (double)nframes / SR;
        double rmsL = sqrt(sumL/nframes), rmsR = sqrt(sumR/nframes);
        /* ゼロ交差は1周期に2回 → freq = zc/2/dur */
        double fL = zcL / 2.0 / dur, fR = zcR / 2.0 / dur;
        fprintf(stderr, "check: frames=%ld dur=%.2fs  L: RMS=%.0f ~%.0fHz  R: RMS=%.0f ~%.0fHz\n",
                nframes, dur, rmsL, fL, rmsR, fR);
        return 0;
    }
    fprintf(stderr, "unknown mode\n");
    return 1;
}
