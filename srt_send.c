/*
 * srt_send.c  ―  マルチコーデック対応 SRT 音声送信(コーデック非依存本体)
 *
 * 標準入力(stdin)の生PCM(S16LE stereo)、または UDP から受けたPCMを、選択したコーデックで
 * エンコードして SRT で送信する。エンコードは srt_codec_api.h の codec_t 越しに呼ぶ。
 *
 * ★無停止コーデック切替(携帯WEBから):
 *   --codec-file=<path> を指定すると、そのファイルに書かれたコーデック名(aac/aptx/aptxhd/
 *   opus/lpcm)を約1秒毎に見て、動作中でも音を止めずにコーデックを切り替える。切替時は
 *   新しいCONFIGを送るので受信側(srt_recv)も自動追従する。
 *   --status-file=<path> を指定すると、現在のコーデック名をそのファイルに書き出す(WEBの確認用)。
 *
 * 読み取りはコーデック非依存のリングバッファに入れる。フレーム長(コーデック毎に異なる)は
 * 送信ループ側だけが持つので、切替時に読み取りスレッドを止める必要がない。
 * 未接続の間はリングが古い順に破棄され、arecord/ALSA の overrun を防ぐ。
 *
 * 使い方:
 *   arecord -f S16_LE -r 48000 -c 2 -t raw \
 *     | ./srt_send <listen_port> [--codec=aac|aptx|aptxhd|opus|lpcm] [--samplerate=48000]
 *        [--channels=2] [--bitrate=96000] [--latency=150] [--passphrase=xxx]
 *        [--udp-in=<port>] [--codec-file=<path>] [--status-file=<path>]
 */
#include <srt/srt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>

#include "srt_codec_api.h"

#define ENC_OUT_CAP 2048
#define RING_CAP (512*1024)   /* 生PCMリング(約1.4秒@48k/16bit/stereo)。未接続時は古い順に破棄。 */

/* ---- コーデック非依存のPCMリングバッファ ---- */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    unsigned char   buf[RING_CAP];
    int             head, tail, count;   /* count=有効バイト数 */
    int             eof;
    long            dropped;             /* 破棄バイト数 */
} g_ring;

static void ring_push(const unsigned char *p, int n) {
    if (n > RING_CAP) { p += n - RING_CAP; n = RING_CAP; }  /* 1回で入りきらない巨大は末尾のみ */
    pthread_mutex_lock(&g_ring.lock);
    if (g_ring.count + n > RING_CAP) {                      /* 満杯: 古い順に捨てる(drop-old) */
        int drop = g_ring.count + n - RING_CAP;
        g_ring.tail = (g_ring.tail + drop) % RING_CAP;
        g_ring.count -= drop; g_ring.dropped += drop;
    }
    int first = RING_CAP - g_ring.head; if (first > n) first = n;
    memcpy(g_ring.buf + g_ring.head, p, first);
    if (n > first) memcpy(g_ring.buf, p + first, n - first);
    g_ring.head = (g_ring.head + n) % RING_CAP;
    g_ring.count += n;
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);
}

/* frame_bytes 取り出す。取れたら1、EOFで取れなければ0。 */
static int ring_pop(unsigned char *out, int frame_bytes) {
    pthread_mutex_lock(&g_ring.lock);
    while (g_ring.count < frame_bytes && !g_ring.eof)
        pthread_cond_wait(&g_ring.cond, &g_ring.lock);
    if (g_ring.count < frame_bytes) { pthread_mutex_unlock(&g_ring.lock); return 0; }
    int first = RING_CAP - g_ring.tail; if (first > frame_bytes) first = frame_bytes;
    memcpy(out, g_ring.buf + g_ring.tail, first);
    if (frame_bytes > first) memcpy(out + first, g_ring.buf, frame_bytes - first);
    g_ring.tail = (g_ring.tail + frame_bytes) % RING_CAP;
    g_ring.count -= frame_bytes;
    pthread_mutex_unlock(&g_ring.lock);
    return 1;
}

typedef struct { int fd; int is_udp; } reader_arg_t;

static void *reader_thread_func(void *arg) {
    reader_arg_t ra = *(reader_arg_t *)arg;
    unsigned char chunk[65536];
    while (1) {
        ssize_t r;
        if (ra.is_udp) r = recv(ra.fd, chunk, sizeof(chunk), 0);
        else           r = read(ra.fd, chunk, sizeof(chunk));
        if (r <= 0) {
            pthread_mutex_lock(&g_ring.lock); g_ring.eof = 1;
            pthread_cond_signal(&g_ring.cond); pthread_mutex_unlock(&g_ring.lock);
            return NULL;
        }
        ring_push(chunk, (int)r);
    }
}

static void die_srt(const char *msg) {
    fprintf(stderr, "[send] SRTエラー: %s : %s\n", msg, srt_getlasterror_str());
    exit(1);
}
static const char *opt_val(const char *arg, const char *key) {
    size_t klen = strlen(key);
    if (strncmp(arg, key, klen) == 0 && arg[klen] == '=') return arg + klen + 1;
    return NULL;
}

/* コーデック選択ファイルを読む。中身の先頭語(aac等)を name[] に。成功=1。 */
static int read_codec_file(const char *path, char *name, int cap) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char line[64]; int ok = 0;
    if (fgets(line, sizeof line, f)) {
        int o = 0;
        for (const char *p = line; *p && o < cap-1; p++) {
            if (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') { if (o>0) break; else continue; }
            name[o++] = *p;
        }
        name[o] = 0; ok = (o > 0);
    }
    fclose(f); return ok;
}
static void write_status_file(const char *path, const char *codec, int latency) {
    FILE *f = fopen(path, "w"); if (!f) return;
    fprintf(f, "{\"codec\":\"%s\",\"latency_ms\":%d}\n", codec, latency);
    fclose(f);
}

/* CONFIG を組み立て直す。config_msg(呼び出し側確保) に書き、長さを返す。 */
static int build_config(unsigned char *config_msg, const codec_t *codec, int samplerate,
                        int channels, const unsigned char *cfg, int cfg_len) {
    config_msg[0] = MSG_TYPE_CONFIG;
    config_msg[1] = (unsigned char)codec->id;
    put_u32le(config_msg + 2, (uint32_t)samplerate);
    config_msg[6] = (unsigned char)channels;
    if (cfg_len > 0) memcpy(config_msg + 7, cfg, cfg_len);
    return 1 + 1 + 4 + 1 + cfg_len;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "使い方: %s <listen_port> [--codec=aac|aptx|aptxhd|opus|lpcm] [--samplerate=48000]\n"
            "        [--channels=2] [--bitrate=96000] [--latency=150] [--passphrase=xxx]\n"
            "        [--udp-in=<port>] [--codec-file=<path>] [--status-file=<path>]\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    const char *codec_name = "aac";
    int samplerate = 48000, channels = 2, bitrate = 96000, latency_ms = 150;
    const char *passphrase = NULL;
    int udp_in_port = 0;
    const char *codec_file = NULL, *status_file = NULL;

    for (int i = 2; i < argc; ++i) {
        const char *v;
        if      ((v = opt_val(argv[i], "--codec")))       codec_name = v;
        else if ((v = opt_val(argv[i], "--samplerate")))  samplerate = atoi(v);
        else if ((v = opt_val(argv[i], "--channels")))    channels   = atoi(v);
        else if ((v = opt_val(argv[i], "--bitrate")))     bitrate    = atoi(v);
        else if ((v = opt_val(argv[i], "--latency")))     latency_ms = atoi(v);
        else if ((v = opt_val(argv[i], "--passphrase")))  passphrase = v;
        else if ((v = opt_val(argv[i], "--udp-in")))      udp_in_port = atoi(v);
        else if ((v = opt_val(argv[i], "--codec-file")))  codec_file = v;
        else if ((v = opt_val(argv[i], "--status-file"))) status_file = v;
        else { fprintf(stderr, "[send] 不明なオプション: %s\n", argv[i]); return 1; }
    }

    /* codec-file があれば初期コーデックはそれを優先 */
    char fcodec[32];
    if (codec_file && read_codec_file(codec_file, fcodec, sizeof fcodec) && codec_get_by_name(fcodec))
        codec_name = fcodec;

    const codec_t *codec = codec_get_by_name(codec_name);
    if (!codec) { fprintf(stderr, "[send] 不正なコーデック: %s\n", codec_name); return 1; }

    int frame_bytes = 0;
    unsigned char codec_cfg[MAX_CONFIG_SIZE]; int codec_cfg_len = 0;
    void *enc = codec->enc_open(samplerate, channels, bitrate, &frame_bytes, codec_cfg, &codec_cfg_len);
    if (!enc) { fprintf(stderr, "[send] コーデック '%s' 初期化失敗。\n", codec->name); return 1; }

    unsigned char *config_msg = malloc(1 + 1 + 4 + 1 + MAX_CONFIG_SIZE);
    int config_len = build_config(config_msg, codec, samplerate, channels, codec_cfg, codec_cfg_len);
    int samples_per_frame = frame_bytes / (channels * (int)sizeof(int16_t));
    int frames_per_100ms = samples_per_frame > 0 ? (samplerate / samples_per_frame) / 10 : 1;
    if (frames_per_100ms < 1) frames_per_100ms = 1;
    fprintf(stderr, "[send] codec=%s(id=%d) frame_bytes=%d\n", codec->name, codec->id, frame_bytes);
    if (status_file) write_status_file(status_file, codec->name, latency_ms);

    /* ---- リング + 読み取りスレッド ---- */
    pthread_mutex_init(&g_ring.lock, NULL); pthread_cond_init(&g_ring.cond, NULL);
    g_ring.head = g_ring.tail = g_ring.count = 0; g_ring.eof = 0; g_ring.dropped = 0;

    reader_arg_t rarg = { .fd = STDIN_FILENO, .is_udp = 0 };
    if (udp_in_port > 0) {
        int ufd = socket(AF_INET, SOCK_DGRAM, 0);
        if (ufd < 0) { perror("[send] udp socket"); return 1; }
        struct sockaddr_in ua; memset(&ua, 0, sizeof ua);
        ua.sin_family = AF_INET; ua.sin_addr.s_addr = INADDR_ANY; ua.sin_port = htons(udp_in_port);
        if (bind(ufd, (struct sockaddr *)&ua, sizeof ua) < 0) { perror("[send] udp bind"); return 1; }
        rarg.fd = ufd; rarg.is_udp = 1;
        fprintf(stderr, "[send] 音声入力: UDP :%d\n", udp_in_port);
    } else fprintf(stderr, "[send] 音声入力: stdin\n");

    pthread_t reader_tid;
    pthread_create(&reader_tid, NULL, reader_thread_func, &rarg);

    /* ---- SRT ---- */
    srt_startup();
    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) die_srt("srt_create_socket");
    int yes = 1;
    srt_setsockopt(sock, 0, SRTO_SENDER, &yes, sizeof yes);
    srt_setsockopt(sock, 0, SRTO_LATENCY, &latency_ms, sizeof latency_ms);
    if (passphrase) {
        size_t pl = strlen(passphrase);
        if (pl < 10 || pl > 79) { fprintf(stderr, "[send] パスフレーズは10〜79文字。\n"); return 1; }
        int kl = 16; srt_setsockopt(sock, 0, SRTO_PASSPHRASE, passphrase, (int)pl);
        srt_setsockopt(sock, 0, SRTO_PBKEYLEN, &kl, sizeof kl);
    }
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr.s_addr = INADDR_ANY;
    if (srt_bind(sock, (struct sockaddr *)&sa, sizeof sa) == SRT_ERROR) die_srt("srt_bind");
    if (srt_listen(sock, 1) == SRT_ERROR) die_srt("srt_listen");
    fprintf(stderr, "[send] ポート %d 待受 (codec=%s latency=%dms)\n", port, codec->name, latency_ms);

    unsigned char *pcm_buf = malloc(ENC_OUT_CAP*8);   /* 最大フレーム長より十分大きく */
    int pcm_cap = ENC_OUT_CAP*8;
    unsigned char *out_buf = malloc(1 + ENC_OUT_CAP); out_buf[0] = MSG_TYPE_AUDIO;

    long total_frames = 0, reconnect = 0;
    int eof_done = 0;
    SRTSOCKET client = SRT_INVALID_SOCK;
    time_t last_poll = 0;

    while (!eof_done) {
        int sa_len = sizeof sa;
        client = srt_accept(sock, (struct sockaddr *)&sa, &sa_len);
        if (client == SRT_INVALID_SOCK) { usleep(500*1000); continue; }
        fprintf(stderr, "[send] 受信側が接続%s。\n", reconnect? "(再)":"");
        codec->enc_reset(enc);
        if (srt_send(client, (char *)config_msg, config_len) == SRT_ERROR) {
            srt_close(client); reconnect++; continue;
        }

        int frame_counter = 0;
        while (1) {
            if (frame_bytes > pcm_cap) { pcm_cap = frame_bytes; pcm_buf = realloc(pcm_buf, pcm_cap); }
            if (!ring_pop(pcm_buf, frame_bytes)) { eof_done = 1; goto conn_done; }

            int out_len = codec->enc_encode(enc, (int16_t *)pcm_buf, frame_bytes, out_buf + 1, ENC_OUT_CAP);
            if (out_len > 0) {
                if (srt_send(client, (char *)out_buf, out_len + 1) == SRT_ERROR) {
                    fprintf(stderr, "[send] 送信エラー(切断): %s\n", srt_getlasterror_str());
                    break;
                }
                total_frames++;
            }
            if (++frame_counter >= frames_per_100ms) {
                frame_counter = 0;
                srt_send(client, (char *)config_msg, config_len);   /* CONFIG定期再送 */
            }

            /* ---- 約1秒毎: コーデック選択ファイルを見て無停止切替 ---- */
            if (codec_file) {
                time_t now = time(NULL);
                if (now != last_poll) {
                    last_poll = now;
                    char want[32];
                    if (read_codec_file(codec_file, want, sizeof want)) {
                        const codec_t *nc = codec_get_by_name(want);
                        if (nc && nc != codec) {
                            int nfb = 0; unsigned char ncfg[MAX_CONFIG_SIZE]; int nlen = 0;
                            void *ne = nc->enc_open(samplerate, channels, bitrate, &nfb, ncfg, &nlen);
                            if (ne) {
                                codec->enc_close(enc);
                                codec = nc; enc = ne; frame_bytes = nfb;
                                memcpy(codec_cfg, ncfg, nlen); codec_cfg_len = nlen;
                                config_len = build_config(config_msg, codec, samplerate, channels, codec_cfg, codec_cfg_len);
                                samples_per_frame = frame_bytes / (channels * (int)sizeof(int16_t));
                                frames_per_100ms = samples_per_frame > 0 ? (samplerate / samples_per_frame) / 10 : 1;
                                if (frames_per_100ms < 1) frames_per_100ms = 1;
                                codec->enc_reset(enc);
                                srt_send(client, (char *)config_msg, config_len);  /* 新CONFIG即送出 */
                                fprintf(stderr, "[send] コーデック切替 → %s (frame_bytes=%d)\n", codec->name, frame_bytes);
                                if (status_file) write_status_file(status_file, codec->name, latency_ms);
                            } else {
                                fprintf(stderr, "[send] '%s' への切替失敗(初期化不可)。継続。\n", want);
                            }
                        }
                    }
                }
            }
        }
        usleep((latency_ms + 300) * 1000);
        srt_close(client); client = SRT_INVALID_SOCK; reconnect++;
    }

conn_done:
    if (client != SRT_INVALID_SOCK) { usleep((latency_ms + 300)*1000); srt_close(client); }
    fprintf(stderr, "[send] 送信完了。frames=%ld reconnect=%ld dropped(bytes)=%ld\n",
            total_frames, reconnect, g_ring.dropped);
    pthread_join(reader_tid, NULL);
    free(pcm_buf); free(out_buf); free(config_msg);
    srt_close(sock); srt_cleanup(); codec->enc_close(enc);
    return 0;
}
