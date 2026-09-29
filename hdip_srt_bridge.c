/*
 * hdip_srt_bridge.c ― HDIP-3000V IP接続モード用 SRTブリッジ(音声のみ・ALG付き)
 *
 * HDIPのIP接続(独自呼制御 UDP23500 + 音声RTP UDP15000)を、フレッツ/VPN上のSRTで
 * 両拠点間に運ぶ。呼制御はASCIIテキスト(キー:値/CR-LF)なので、各Bridgeが自分の隣の
 * HDIPへ渡す電文中の SRC-ADDR / CONTACT-ADDR / MEDIA-ADDR を「自分(Bridge)のIP」に
 * 書き換える(ALG)。これによりHDIPは制御応答もRTPも手元Bridgeへ送り、RTPがSRTを通る。
 *
 *   HDIP-A ⇄ [Bridge-A] ══SRT══ [Bridge-B] ⇄ HDIP-B
 *
 * SRT ON/OFF・latency・オプションキーは HDIPのWEB画面(Audio→SRT)で設定し、
 * Bridgeが http://<dev-ip>/script/srtsetting.py を定期的に読んで反映する。
 *   SRT ON : 有効なオプションキー(SRT(IP接続)を含む・HDIPのMAC専用) かつ SRT=ON
 *   SRT OFF: それ以外。送りは素のUDP(udp-port)で相手Bridgeへ(ALGは同じ)。
 *   SRT ONでもSRTがつながるまで(切替直後・再接続中)は素のUDPで送り、接続直後は
 *   latency+0.3秒だけ音声をUDPとSRTの両方で送り、SRTを閉じる時は直近latency+0.2秒分をUDPで送り直す
 *   (受け側はRTPシーケンス番号で重複除去)=ON/OFF・latency切替で音を切らない。
 * 受けは常にSRTとUDPの両方を待つので、両拠点の設定が食い違っても音は通る
 * (各方向は送り側の設定に従う)。
 *
 * 使い方(両拠点で tx/rx を入れ替えて実行):
 *   ./hdip_srt_bridge --peer=<相手BridgeIP> --tx-port=9000 --rx-port=9001 \
 *       --local-ip=<自BridgeのHDIP側IP> --dev-ip=<自拠点HDIPのIP> \
 *       [--udp-port=9100] [--peer-udp-port(試験用・既定=udp-port)] [--web-port=80] [--poll=3] [--latency=250(WEB値が不正な時の既定)] \
 *       [--ctl-port=23500] [--media-port=15000] [--passphrase=xxx]
 *
 * 多重: 1メッセージ先頭1バイトのタグ  0x01=制御(23500) / 0x02=RTP(15000)。SRT/UDP共通。
 * 音声のみ対応(映像V-PORT/制御C-PORT/インカムI-PORTは運ばない)。
 */
#include <srt/srt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdint.h>
#include <time.h>
#include <arpa/inet.h>

#define TAG_CTL 0x01
#define TAG_MED 0x02
#define BUFMAX  2100
#define SRT_PAYLOAD 1456

static int   g_ctl_port=23500, g_media_port=15000, g_latency_default=250;
static char  g_local_ip[64]="", g_dev_ip[64]="", g_peer[64]="";
static int   g_tx_port=9000, g_rx_port=9001, g_udp_port=9100, g_peer_udp_port=0;  /* 0=udp-portと同じ */
static int   g_web_port=80, g_poll_s=3;
static const char *g_pass=NULL;

static int   g_ctl_fd=-1, g_med_fd=-1;          /* HDIP側UDP(受信bind + 送信) */
static int   g_peer_udp_fd=-1;                  /* SRT OFF時の相手BridgeとのUDP */
static struct sockaddr_in g_dev_ctl, g_dev_med, g_peer_udp;

/* 現在の動作設定。変わるたびに g_gen を進め、各スレッドは張り直す */
static pthread_mutex_t g_mx=PTHREAD_MUTEX_INITIALIZER;
static int g_srt_on=0, g_latency=250, g_gen=0;

static void cur_cfg(int*on,int*lat,int*gen){
    pthread_mutex_lock(&g_mx); *on=g_srt_on; *lat=g_latency; *gen=g_gen; pthread_mutex_unlock(&g_mx);
}
static int gen_now(void){ int g; pthread_mutex_lock(&g_mx); g=g_gen; pthread_mutex_unlock(&g_mx); return g; }

/* ログ: 時刻(時:分:秒.ミリ秒)付き */
static void log_ts(void){ struct timeval tv; gettimeofday(&tv,NULL); struct tm tm; localtime_r(&tv.tv_sec,&tm);
    fprintf(stderr,"%02d:%02d:%02d.%03ld [hdip] ",tm.tm_hour,tm.tm_min,tm.tm_sec,(long)tv.tv_usec/1000); }
#define LOG(...) do{ log_ts(); fprintf(stderr,__VA_ARGS__); }while(0)

static const char *opt(const char*a,const char*k){ size_t n=strlen(k); if(strncmp(a,k,n)==0&&a[n]=='=') return a+n+1; return NULL; }

/* 呼制御テキストの SRC-ADDR/CONTACT-ADDR/MEDIA-ADDR の値を local_ip に置換。
   1行=「キー:値」CR/LF区切り。戻り値=出力長。 */
static int rewrite_ctrl(const char*in,int inlen,char*out,int outcap,const char*ip){
    static const char *keys[]={"SRC-ADDR","CONTACT-ADDR","MEDIA-ADDR"};
    int oi=0,i=0;
    while(i<inlen){
        int start=i;
        while(i<inlen && in[i]!='\n') i++;
        int has_nl = (i<inlen);
        int content_end = i;                   /* '\n'の手前 */
        int has_cr = (content_end>start && in[content_end-1]=='\r');
        int body_end = has_cr ? content_end-1 : content_end;
        /* キー判定 */
        int matched=-1;
        for(int k=0;k<3;k++){ int kl=(int)strlen(keys[k]);
            if(start+kl<inlen && strncmp(in+start,keys[k],kl)==0 && in[start+kl]==':'){ matched=k; break; } }
        if(matched>=0){
            oi += snprintf(out+oi, outcap-oi, "%s:%s%s", keys[matched], ip, has_cr?"\r":"");
        } else {
            int blen = body_end-start + (has_cr?1:0);   /* 本文(+CR) をそのままコピー */
            if(oi+blen<outcap){ memcpy(out+oi,in+start,blen); oi+=blen; }
        }
        if(has_nl){ if(oi<outcap) out[oi++]='\n'; i++; }
    }
    return oi;
}

static void die(const char*m){ LOG("%s: %s\n",m,srt_getlasterror_str()); exit(1); }

static long now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000L+t.tv_nsec/1000000; }

/* RTP重複除去: SRT切替直後は同じRTPがUDPとSRTの両方で届くので、2秒以内に渡した
   (SSRC,シーケンス番号)は捨てる。RTPでないもの(v!=2/短い)は素通し。重複=1 */
#define DUP_WINDOW_MS 2000
static pthread_mutex_t g_dup_mx=PTHREAD_MUTEX_INITIALIZER;
static long     g_dup_ms[65536];
static uint32_t g_dup_ssrc[65536];
static int rtp_duplicate(const unsigned char*p,int plen){
    if(plen<12 || (p[0]>>6)!=2) return 0;
    unsigned seq=((unsigned)p[2]<<8)|p[3];
    uint32_t ssrc=((uint32_t)p[8]<<24)|((uint32_t)p[9]<<16)|((uint32_t)p[10]<<8)|p[11];
    long t=now_ms(); int dup;
    pthread_mutex_lock(&g_dup_mx);
    dup = g_dup_ms[seq] && g_dup_ssrc[seq]==ssrc && t-g_dup_ms[seq]<DUP_WINDOW_MS;
    if(!dup){ g_dup_ms[seq]=t; g_dup_ssrc[seq]=ssrc; }
    pthread_mutex_unlock(&g_dup_mx);
    return dup;
}

/* 相手から届いた1メッセージ(タグ付き)をHDIPへ。制御はALG書換。SRT/UDPの受けスレッド共通 */
static void deliver_to_dev(const unsigned char*buf,int n){
    char rw[BUFMAX];
    if(n<=1) return;
    const unsigned char*p=buf+1; int plen=n-1;
    if(buf[0]==TAG_CTL){
        int m=rewrite_ctrl((const char*)p,plen,rw,BUFMAX,g_local_ip);
        sendto(g_ctl_fd,rw,m,0,(struct sockaddr*)&g_dev_ctl,sizeof g_dev_ctl);
    } else if(buf[0]==TAG_MED){
        if(rtp_duplicate(p,plen)) return;
        sendto(g_med_fd,p,plen,0,(struct sockaddr*)&g_dev_med,sizeof g_dev_med);
    }
}

/* HDIPから1パケット読んでタグを付ける(k=0:制御 1:RTP)。戻り値=タグ込み長(0=無し) */
static int read_from_dev(int k,unsigned char*buf){
    int n=recv(k?g_med_fd:g_ctl_fd,buf+1,BUFMAX-1,0);
    if(n<=0) return 0;
    buf[0]= k?TAG_MED:TAG_CTL;
    return n+1;
}

/* SRTで送った直近の音声(送りスレッドだけが使う)。最大latency 2000ms+α ≒ 375パケット */
#define RECENT_MAX 512
static unsigned char g_recent[RECENT_MAX][BUFMAX];
static int  g_recent_len[RECENT_MAX], g_recent_n=0;
static long g_recent_ms[RECENT_MAX];
static void recent_push(const unsigned char*buf,int n){
    int i=g_recent_n%RECENT_MAX;
    memcpy(g_recent[i],buf,n); g_recent_len[i]=n; g_recent_ms[i]=now_ms(); g_recent_n++;
}
static void recent_resend_udp(long since_ms){
    int cnt = g_recent_n<RECENT_MAX ? g_recent_n : RECENT_MAX;
    for(int j=g_recent_n-cnt;j<g_recent_n;j++){ int i=j%RECENT_MAX;
        if(g_recent_ms[i]>=since_ms) sendto(g_peer_udp_fd,g_recent[i],g_recent_len[i],0,(struct sockaddr*)&g_peer_udp,sizeof g_peer_udp); }
    g_recent_n=0;
}

/* ---- 送り(SRT ON): HDIP → SRT(listen tx-port)。設定が変わったら戻る ---- */
static void run_srt_out(int gen,int lat){
    SRTSOCKET s=srt_create_socket();
    int yes=1,pl=SRT_PAYLOAD;
    srt_setsockopt(s,0,SRTO_SENDER,&yes,sizeof yes);
    srt_setsockopt(s,0,SRTO_LATENCY,&lat,sizeof lat);
    srt_setsockopt(s,0,SRTO_PAYLOADSIZE,&pl,sizeof pl);
    if(g_pass){ int kl=16; srt_setsockopt(s,0,SRTO_PASSPHRASE,g_pass,(int)strlen(g_pass)); srt_setsockopt(s,0,SRTO_PBKEYLEN,&kl,sizeof kl); }
    struct sockaddr_in sa; memset(&sa,0,sizeof sa); sa.sin_family=AF_INET; sa.sin_addr.s_addr=INADDR_ANY; sa.sin_port=htons(g_tx_port);
    if(srt_bind(s,(struct sockaddr*)&sa,sizeof sa)==SRT_ERROR){
        LOG("srt_bind(tx):%s(再試行)\n",srt_getlasterror_str()); srt_close(s); usleep(1000000); return; }
    if(srt_listen(s,1)==SRT_ERROR) die("srt_listen(tx)");
    LOG("送り=SRT: HDIP(:%d,:%d) → SRT listen :%d latency=%dms\n",g_ctl_port,g_media_port,g_tx_port,lat);

    unsigned char buf[BUFMAX];
    struct pollfd pf[2]={{g_ctl_fd,POLLIN,0},{g_med_fd,POLLIN,0}};
    /* acceptで止まらないよう(設定変更を見るため)、接続要求はepollで200ms毎に確認 */
    int eid=srt_epoll_create(), ev=SRT_EPOLL_IN;
    srt_epoll_add_usock(eid,s,&ev);
    while(gen_now()==gen){
        SRTSOCKET rd[1]; int nrd=1;
        if(srt_epoll_wait(eid,rd,&nrd,NULL,NULL,0,NULL,NULL,NULL,NULL)<=0){
            /* SRTがつながるまでは素のUDPで送る(ON切替直後・SRT再接続中も音を切らない) */
            if(poll(pf,2,200)>0) for(int k=0;k<2;k++) if(pf[k].revents&POLLIN){
                int n=read_from_dev(k,buf);
                if(n) sendto(g_peer_udp_fd,buf,n,0,(struct sockaddr*)&g_peer_udp,sizeof g_peer_udp);
            }
            continue;
        }
        int al=sizeof sa; SRTSOCKET c=srt_accept(s,(struct sockaddr*)&sa,&al);
        if(c==SRT_INVALID_SOCK) continue;
        LOG("送りSRT: 相手接続\n");
        /* 接続直後のSRTは最初の約latency分を捨てることがあるので、その間は音声をUDPでも送る。
           閉じる時はSRTで未着かもしれない直近 latency+0.2秒分の音声をUDPで送り直す。
           (どちらも受け側がRTPシーケンス番号で重複除去) */
        /* SRT送信で止まらない(接続直後にsrt_sendが待たされることがある)。
           SRTが受け付けない間はUDPで送り、その後 latency+0.3秒はUDPとの二重送信を続ける */
        int no=0; srt_setsockopt(c,0,SRTO_SNDSYN,&no,sizeof no);
        long overlap_until=now_ms()+lat+300;
        int broken=0;
        g_recent_n=0;
        while(!broken && gen_now()==gen){
            if(poll(pf,2,200)<=0) continue;
            for(int k=0;k<2;k++) if(pf[k].revents&POLLIN){
                int n=read_from_dev(k,buf); if(!n) continue;
                int udp_sent=0;
                if(k==1){
                    if(now_ms()<overlap_until){ sendto(g_peer_udp_fd,buf,n,0,(struct sockaddr*)&g_peer_udp,sizeof g_peer_udp); udp_sent=1; }
                    recent_push(buf,n);
                }
                if(srt_send(c,(char*)buf,n)==SRT_ERROR){
                    if(srt_getlasterror(NULL)!=SRT_EASYNCSND){ LOG("送りSRT切断\n"); broken=1; break; }
                    /* SRTがまだ受け付けない → このパケットはUDPで送り、二重送信期間を延ばす */
                    if(!udp_sent) sendto(g_peer_udp_fd,buf,n,0,(struct sockaddr*)&g_peer_udp,sizeof g_peer_udp);
                    overlap_until=now_ms()+lat+300;
                }
            }
            if(getenv("HDIP_SRT_STATS")){ static long last=0; long t=now_ms(); if(t-last>=250){ last=t; SRT_TRACEBSTATS st; srt_bstats(c,&st,1);
                LOG("SSTAT sent=%ld snddrop=%d retrans=%d sndbuf=%dpkt/%dms loss=%d rtt=%.1f\n",(long)st.pktSent,st.pktSndDrop,st.pktRetrans,st.pktSndBuf,st.msSndBuf,st.pktSndLoss,st.msRTT); } }
        }
        srt_close(c);
        recent_resend_udp(now_ms()-lat-200);
    }
    srt_epoll_release(eid);
    srt_close(s);
}

/* ---- 送り(SRT OFF): HDIP → 素のUDPで相手Bridge(udp-port)へ。設定が変わったら戻る ---- */
static void run_udp_out(int gen){
    LOG("送り=UDP(SRT OFF): HDIP(:%d,:%d) → %s:%d\n",g_ctl_port,g_media_port,g_peer,ntohs(g_peer_udp.sin_port));
    unsigned char buf[BUFMAX];
    struct pollfd pf[2]={{g_ctl_fd,POLLIN,0},{g_med_fd,POLLIN,0}};
    while(gen_now()==gen){
        if(poll(pf,2,200)<=0) continue;
        for(int k=0;k<2;k++) if(pf[k].revents&POLLIN){
            int n=read_from_dev(k,buf); if(!n) continue;
            sendto(g_peer_udp_fd,buf,n,0,(struct sockaddr*)&g_peer_udp,sizeof g_peer_udp);
        }
    }
}

static void *outbound_thread(void *arg){
    (void)arg;
    while(1){
        int on,lat,gen; cur_cfg(&on,&lat,&gen);
        if(on) run_srt_out(gen,lat); else run_udp_out(gen);
    }
    return NULL;
}

/* ---- 受け(SRT): SRT(connect rx-port) → ALG書換 → HDIP。常時動作 ---- */
static void *inbound_srt_thread(void *arg){
    (void)arg;
    struct sockaddr_in pa; memset(&pa,0,sizeof pa); pa.sin_family=AF_INET; pa.sin_port=htons(g_rx_port);
    inet_pton(AF_INET,g_peer,&pa.sin_addr);
    unsigned char buf[BUFMAX];
    int quiet=0;   /* 相手がSRT OFFの間は接続失敗が続くのでログは1回だけ */
    while(1){
        int on,lat,gen; cur_cfg(&on,&lat,&gen);
        SRTSOCKET s=srt_create_socket();
        int pl=SRT_PAYLOAD, tmo=500;
        srt_setsockopt(s,0,SRTO_LATENCY,&lat,sizeof lat);
        srt_setsockopt(s,0,SRTO_PAYLOADSIZE,&pl,sizeof pl);
        srt_setsockopt(s,0,SRTO_RCVTIMEO,&tmo,sizeof tmo);
        int cto=1500; srt_setsockopt(s,0,SRTO_CONNTIMEO,&cto,sizeof cto);   /* 相手のON切替に早く追従 */
        if(g_pass) srt_setsockopt(s,0,SRTO_PASSPHRASE,g_pass,(int)strlen(g_pass));
        if(srt_connect(s,(struct sockaddr*)&pa,sizeof pa)==SRT_ERROR){
            if(!quiet) LOG("受けSRT未接続(%s): 相手がSRT OFFの間はUDPで受信\n",srt_getlasterror_str());
            quiet=1; srt_close(s); usleep(500000); continue;
        }
        quiet=0;
        LOG("受けSRT: %s:%d へ接続 latency=%dms\n",g_peer,g_rx_port,lat);
        while(1){
            int n=srt_recv(s,(char*)buf,BUFMAX);
            if(n==SRT_ERROR){
                int e=srt_getlasterror(NULL);
                if(e==SRT_ETIMEOUT||e==SRT_EASYNCRCV){ if(gen_now()!=gen) break; continue; }  /* 受信タイムアウト */
                LOG("受けSRT切断(再接続)\n"); break;
            }
            deliver_to_dev(buf,n);
            if(getenv("HDIP_SRT_STATS")){ static long last=0; long t=now_ms(); if(t-last>=250){ last=t; SRT_TRACEBSTATS st; srt_bstats(s,&st,1);
                LOG("STAT rcv=%ld loss=%d drop=%d belated=%ld retrans(recv)=%d bufms=%d rtt=%.1f\n",(long)st.pktRecv,st.pktRcvLoss,st.pktRcvDrop,(long)st.pktRcvBelated,st.pktRcvRetrans,st.msRcvBuf,st.msRTT); } }
            if(gen_now()!=gen) break;   /* latency変更などで張り直し */
        }
        srt_close(s); usleep(200000);
    }
    return NULL;
}

/* ---- 受け(UDP): 相手がSRT OFFの時の素のUDP → ALG書換 → HDIP。常時動作 ---- */
static void *inbound_udp_thread(void *arg){
    (void)arg;
    unsigned char buf[BUFMAX];
    struct in_addr peer; inet_pton(AF_INET,g_peer,&peer);
    while(1){
        struct sockaddr_in from; socklen_t fl=sizeof from;
        int n=recvfrom(g_peer_udp_fd,buf,BUFMAX,0,(struct sockaddr*)&from,&fl);
        if(n<=0) continue;
        if(from.sin_addr.s_addr!=peer.s_addr) continue;   /* 相手Bridge以外からは受けない */
        deliver_to_dev(buf,n);
    }
    return NULL;
}

/* ---- HDIPのWEB(script/srtsetting.py)からSRT設定を取得 ---- */

/* JSON本文から "name": "値" の値を取り出す(srtsetting.pyの出力専用の簡易版)。成功=0 */
static int json_str(const char*body,const char*name,char*out,int cap){
    char pat[64]; snprintf(pat,sizeof pat,"\"%s\"",name);
    const char*p=strstr(body,pat); if(!p) return -1;
    p+=strlen(pat); while(*p==' ') p++;
    if(*p!=':') return -1;
    p++; while(*p==' ') p++;
    if(*p!='"') return -1;
    p++;
    int o=0; while(*p&&*p!='"'&&o<cap-1) out[o++]=*p++;
    out[o]=0; return *p=='"'?0:-1;
}

/* HTTP GET。成功=本文長(outに本文)、失敗=-1 */
static int http_get(const char*ip,int port,const char*path,char*out,int cap){
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
    struct timeval tv={2,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);   /* connectにも効く */
    struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(port);
    inet_pton(AF_INET,ip,&a.sin_addr);
    if(connect(fd,(struct sockaddr*)&a,sizeof a)<0){ close(fd); return -1; }
    char req[256]; int rl=snprintf(req,sizeof req,"GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",path,ip);
    if(send(fd,req,rl,0)!=rl){ close(fd); return -1; }
    int n=0,r;
    while(n<cap-1 && (r=recv(fd,out+n,cap-1-n,0))>0) n+=r;
    close(fd); out[n]=0;
    if(n<12 || strncmp(out,"HTTP/1.",7)!=0 || strncmp(out+9,"200",3)!=0) return -1;
    char*body=strstr(out,"\r\n\r\n"); if(!body) return -1;
    body+=4; int bl=(int)(out+n-body); memmove(out,body,bl+1);
    return bl;
}

static void *config_thread(void *arg){
    (void)arg;
    char body[8192], en[8], lat[16], why[160], lastwhy[160]="";
    int fail=-1;   /* -1=未取得 0=取得中 1=失敗中 */
    while(1){
        int ok=http_get(g_dev_ip,g_web_port,"/script/srtsetting.py",body,sizeof body)>0
            && json_str(body,"enable",en,sizeof en)==0
            && json_str(body,"latency_ms",lat,sizeof lat)==0;
        if(!ok){
            /* 取得失敗中は直前の設定を維持(一時的なWEB無応答で音を切らない) */
            if(fail!=1) LOG("HDIPのSRT設定を取得できません(http://%s:%d/script/srtsetting.py) → %s\n",
                                g_dev_ip,g_web_port, fail<0?"SRT OFFで開始":"直前の設定を維持");
            fail=1; sleep(g_poll_s); continue;
        }
        if(fail==1) LOG("HDIPのSRT設定を再取得しました\n");
        fail=0;
        /* 自社利用のためライセンス検証は行わない。SRTのON/OFFはWEB設定のenableのみで決まる。 */
        int want_on = 0;
        if(strcmp(en,"1")!=0)  snprintf(why,sizeof why,"WEB設定でSRT OFF");
        else { want_on=1;      snprintf(why,sizeof why,"SRT ON"); }
        int want_lat=atoi(lat); if(want_lat<20||want_lat>2000) want_lat=g_latency_default;

        pthread_mutex_lock(&g_mx);
        int changed = (want_on!=g_srt_on || want_lat!=g_latency);
        if(changed){ g_srt_on=want_on; g_latency=want_lat; g_gen++; }
        pthread_mutex_unlock(&g_mx);
        if(changed || strcmp(why,lastwhy)!=0)
            LOG("%s: %s  latency=%dms\n",changed?"設定変更":"設定",why,want_lat);
        snprintf(lastwhy,sizeof lastwhy,"%s",why);
        sleep(g_poll_s);
    }
    return NULL;
}

static int bind_udp(const char*ip,int port){
    int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd<0){perror("socket");exit(1);}
    int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_port=htons(port);
    /* ipが未指定/不正ならANY */
    if(!ip||inet_pton(AF_INET,ip,&a.sin_addr)!=1) a.sin_addr.s_addr=INADDR_ANY;
    if(bind(fd,(struct sockaddr*)&a,sizeof a)<0){ perror("bind"); exit(1); }
    return fd;
}

int main(int argc,char**argv){
    for(int i=1;i<argc;i++){ const char*v;
        if((v=opt(argv[i],"--peer"))) strncpy(g_peer,v,sizeof g_peer-1);
        else if((v=opt(argv[i],"--tx-port"))) g_tx_port=atoi(v);
        else if((v=opt(argv[i],"--rx-port"))) g_rx_port=atoi(v);
        else if((v=opt(argv[i],"--udp-port"))) g_udp_port=atoi(v);
        else if((v=opt(argv[i],"--peer-udp-port"))) g_peer_udp_port=atoi(v);
        else if((v=opt(argv[i],"--local-ip"))) strncpy(g_local_ip,v,sizeof g_local_ip-1);
        else if((v=opt(argv[i],"--dev-ip"))) strncpy(g_dev_ip,v,sizeof g_dev_ip-1);
        else if((v=opt(argv[i],"--ctl-port"))) g_ctl_port=atoi(v);
        else if((v=opt(argv[i],"--media-port"))) g_media_port=atoi(v);
        else if((v=opt(argv[i],"--web-port"))) g_web_port=atoi(v);
        else if((v=opt(argv[i],"--poll"))) g_poll_s=atoi(v)>0?atoi(v):3;
        else if((v=opt(argv[i],"--latency"))) g_latency_default=atoi(v);
        else if((v=opt(argv[i],"--passphrase"))) g_pass=v;
        else { fprintf(stderr,"不明: %s\n",argv[i]); return 1; }
    }
    if(!*g_peer||!*g_local_ip||!*g_dev_ip){
        fprintf(stderr,"必須: --peer, --local-ip(自BridgeのHDIP側IP), --dev-ip(自拠点HDIP)\n");
        return 1;
    }
    g_latency=g_latency_default;
    g_ctl_fd=bind_udp(g_local_ip,g_ctl_port);
    g_med_fd=bind_udp(g_local_ip,g_media_port);
    g_peer_udp_fd=bind_udp(NULL,g_udp_port);
    memset(&g_dev_ctl,0,sizeof g_dev_ctl); g_dev_ctl.sin_family=AF_INET; g_dev_ctl.sin_port=htons(g_ctl_port); inet_pton(AF_INET,g_dev_ip,&g_dev_ctl.sin_addr);
    memset(&g_dev_med,0,sizeof g_dev_med); g_dev_med.sin_family=AF_INET; g_dev_med.sin_port=htons(g_media_port); inet_pton(AF_INET,g_dev_ip,&g_dev_med.sin_addr);
    memset(&g_peer_udp,0,sizeof g_peer_udp); g_peer_udp.sin_family=AF_INET; g_peer_udp.sin_port=htons(g_peer_udp_port?g_peer_udp_port:g_udp_port); inet_pton(AF_INET,g_peer,&g_peer_udp.sin_addr);

    fprintf(stderr,"=== HDIP SRT Bridge (音声のみ/ALG/WEB設定連動) ===\n");
    fprintf(stderr,"  local-ip(書換先)=%s  dev(HDIP)=%s  peer=%s tx=%d rx=%d udp=%d\n",
            g_local_ip,g_dev_ip,g_peer,g_tx_port,g_rx_port,g_udp_port);
    fprintf(stderr,"  SRT設定: http://%s:%d/script/srtsetting.py を%d秒毎に確認\n",g_dev_ip,g_web_port,g_poll_s);
    fprintf(stderr,"  ALG: SRC-ADDR/CONTACT-ADDR/MEDIA-ADDR → %s\n",g_local_ip);

    srt_startup();

    pthread_t t[4];
    pthread_create(&t[0],NULL,config_thread,NULL);
    pthread_create(&t[1],NULL,inbound_srt_thread,NULL);
    pthread_create(&t[2],NULL,inbound_udp_thread,NULL);
    pthread_create(&t[3],NULL,outbound_thread,NULL);
    for(int i=0;i<4;i++) pthread_join(t[i],NULL);
    return 0;
}
