/*
 * udp_proxy.c  ―  劣化模擬UDPプロキシ(ロス/遅延/ジッタ)。SRT経路に挟んで耐性試験に使う。
 *
 *   caller(srt2udp) ⇄ [udp_proxy] ⇄ listener(udp2srt)
 *
 * 双方向のUDPを中継し、ロス率・遅延・ジッタを与える。SRTは単一UDPフロー上で
 * データ/制御を双方向にやり取りするので、本プロキシは両方向を中継する。
 *
 * 使い方:
 *   ./udp_proxy --listen=<port> --forward=<host:port> [--loss=%] [--delay=ms] [--jitter=ms]
 *     --listen  : caller(srt2udp)がここへ接続
 *     --forward : listener(udp2srt)の実SRTアドレス
 *
 * 例(ロス10%/遅延80ms/ジッタ±30ms):
 *   ./udp_proxy --listen=9600 --forward=127.0.0.1:9500 --loss=10 --delay=80 --jitter=30
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define PKT_MAX 1600
#define QCAP   16384

typedef struct { unsigned char buf[PKT_MAX]; int len; long due_ms; int to_caller; int used; } pend_t;
static pend_t Q[QCAP];

static long now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1000L+ts.tv_nsec/1000000L; }
static const char *opt(const char*a,const char*k){ size_t n=strlen(k); if(strncmp(a,k,n)==0&&a[n]=='=') return a+n+1; return NULL; }

int main(int argc,char**argv){
    int listen_port=0, loss=0, delay=0, jitter=0;
    char fhost[256]={0}; int fport=0;
    for(int i=1;i<argc;i++){ const char*v;
        if((v=opt(argv[i],"--listen"))) listen_port=atoi(v);
        else if((v=opt(argv[i],"--forward"))){ const char*c=strrchr(v,':'); if(!c){fprintf(stderr,"--forward host:port\n");return 1;} size_t hl=c-v; if(hl>=sizeof fhost)hl=sizeof fhost-1; memcpy(fhost,v,hl); fhost[hl]=0; fport=atoi(c+1);}
        else if((v=opt(argv[i],"--loss"))) loss=atoi(v);
        else if((v=opt(argv[i],"--delay"))) delay=atoi(v);
        else if((v=opt(argv[i],"--jitter"))) jitter=atoi(v);
        else { fprintf(stderr,"不明: %s\n",argv[i]); return 1; }
    }
    if(!listen_port||!fport){ fprintf(stderr,"--listen と --forward は必須\n"); return 1; }

    int a=socket(AF_INET,SOCK_DGRAM,0);   /* caller側 */
    int b=socket(AF_INET,SOCK_DGRAM,0);   /* listener側 */
    struct sockaddr_in la; memset(&la,0,sizeof la); la.sin_family=AF_INET; la.sin_addr.s_addr=INADDR_ANY; la.sin_port=htons(listen_port);
    if(bind(a,(struct sockaddr*)&la,sizeof la)<0){ perror("bind"); return 1; }
    struct sockaddr_in fa; memset(&fa,0,sizeof fa); fa.sin_family=AF_INET; fa.sin_port=htons(fport);
    if(inet_pton(AF_INET,fhost,&fa.sin_addr)!=1){ fprintf(stderr,"forward IP不正\n"); return 1; }

    struct sockaddr_in caller; int have_caller=0; socklen_t sl;
    srand((unsigned)time(NULL));
    fprintf(stderr,"[proxy] :%d ⇄ %s:%d  loss=%d%% delay=%dms jitter=±%dms\n",listen_port,fhost,fport,loss,delay,jitter);

    long dropped=0, passed=0;
    while(1){
        /* 次に送出すべき時刻までのタイムアウトを計算 */
        long t=now_ms(); long wait=1000;
        for(int i=0;i<QCAP;i++) if(Q[i].used){ long d=Q[i].due_ms-t; if(d<wait) wait=d; }
        if(wait<0) wait=0;
        struct pollfd pf[2]={{a,POLLIN,0},{b,POLLIN,0}};
        poll(pf,2,(int)wait);

        t=now_ms();
        /* caller(a)から受信 → listenerへ向かうパケット(to_caller=0) */
        if(pf[0].revents&POLLIN){ unsigned char buf[PKT_MAX]; struct sockaddr_in from; sl=sizeof from;
            int n=recvfrom(a,buf,sizeof buf,0,(struct sockaddr*)&from,&sl);
            if(n>0){ caller=from; have_caller=1;
                if(loss>0 && (rand()%100)<loss){ dropped++; }
                else { int j=jitter? (rand()%(2*jitter+1))-jitter :0; long due=t+delay+j; if(due<t)due=t;
                    for(int i=0;i<QCAP;i++) if(!Q[i].used){ memcpy(Q[i].buf,buf,n); Q[i].len=n; Q[i].due_ms=due; Q[i].to_caller=0; Q[i].used=1; break; } }
            }
        }
        /* listener(b)から受信 → callerへ向かうパケット(to_caller=1) */
        if(pf[1].revents&POLLIN){ unsigned char buf[PKT_MAX]; struct sockaddr_in from; sl=sizeof from;
            int n=recvfrom(b,buf,sizeof buf,0,(struct sockaddr*)&from,&sl);
            if(n>0){
                if(loss>0 && (rand()%100)<loss){ dropped++; }
                else { int j=jitter? (rand()%(2*jitter+1))-jitter :0; long due=t+delay+j; if(due<t)due=t;
                    for(int i=0;i<QCAP;i++) if(!Q[i].used){ memcpy(Q[i].buf,buf,n); Q[i].len=n; Q[i].due_ms=due; Q[i].to_caller=1; Q[i].used=1; break; } }
            }
        }
        /* 期限が来たパケットを送出 */
        t=now_ms();
        for(int i=0;i<QCAP;i++) if(Q[i].used && Q[i].due_ms<=t){
            if(Q[i].to_caller){ if(have_caller) sendto(a,Q[i].buf,Q[i].len,0,(struct sockaddr*)&caller,sizeof caller); }
            else              { sendto(b,Q[i].buf,Q[i].len,0,(struct sockaddr*)&fa,sizeof fa); }
            Q[i].used=0; passed++;
        }
    }
    (void)passed;(void)dropped;
    return 0;
}
