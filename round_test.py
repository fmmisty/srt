#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
累積稼働テスト(3分 x 10ラウンド = 累積30分)

- 送信側(aac_srt_send)・受信側(aac_srt_recv)を実際に起動し、
  440Hz/660Hzのステレオテストトーンを送り続ける
- 各ラウンド(180秒)の90秒目に受信側プロセスをSIGKILLで強制切断し、
  自動再接続を確認する
- 5秒おきに送受信両方のRSS(常駐メモリ)を記録し、メモリリークがないか確認する
- 実行のたびに1ラウンドずつ進む「再開可能」な作りになっている。
  10ラウンド終わるまで、このスクリプトを繰り返し実行すればよい
  (1回のプロセス強制終了やタイムアウトで進捗が失われないようにするため)。

■ 使い方(このzipを展開した srt_audio_final/ ディレクトリ内で)
    python3 round_test.py
  を10回実行する(各回 約3分強かかる)。合計で約10回、30分強。

■ 出力
    round_rss.csv               … 5秒おきのRSS推移(累積)
    round_reconnect_events.txt  … 各ラウンドの再接続結果
    round_send.log / round_recv.log … 送受信プロセスのログ(累積)
    round_decoded.raw           … 受信側がデコードしたPCM(累積、S16LE/48kHz/stereo)
    round_state.txt             … 進捗(次に実行すべきラウンド番号)

■ 最初からやり直したい場合
    rm -f round_state.txt round_rss.csv round_reconnect_events.txt \
          round_send.log round_recv.log round_decoded.raw
  を実行してから再度 python3 round_test.py を実行する。

■ 前提
    BUILD.md の手順通りに libsrt / fdk-aac をビルド・インストール済みで、
    このディレクトリで `make` して aac_srt_send / aac_srt_recv が
    ビルドされていること。
"""
import subprocess
import time
import os
import struct
import math
import signal
import threading

# ---- 設定 ----
ROUND_SEC = 180        # 1ラウンドの長さ(秒)
TOTAL_ROUNDS = 10       # 累積目標ラウンド数
KILL_AT = 90            # 各ラウンドの何秒目で受信側を強制切断するか
SAMPLE_INTERVAL = 5     # RSSサンプリング間隔(秒)
PORT = "9800"
PASSPHRASE = "RoundTestPass20260703"   # このテスト専用のパスフレーズ(本番用とは別物)
TONE_SEC = 5            # ループ再生するテストトーンの長さ(秒)

# このスクリプトが置かれているディレクトリを基準にする(会社PCでもそのまま動くように)
WORKDIR = os.path.dirname(os.path.abspath(__file__))
STATE_FILE = os.path.join(WORKDIR, "round_state.txt")
TONE_FILE = os.path.join(WORKDIR, "round_tone_5s.raw")

env = dict(os.environ)
# ビルド時に /usr/local/lib にインストールした libsrt/fdk-aac を確実に見つけられるようにする
env["LD_LIBRARY_PATH"] = "/usr/local/lib:" + env.get("LD_LIBRARY_PATH", "")

os.chdir(WORKDIR)


def ensure_tone_file():
    """440Hz(L)/660Hz(R)のステレオテストトーンを生成する(初回のみ)。"""
    if os.path.exists(TONE_FILE):
        return
    sr = 48000
    n = sr * TONE_SEC
    data = bytearray()
    for i in range(n):
        t = i / sr
        l = int(10000 * math.sin(2 * math.pi * 440 * t))
        r = int(10000 * math.sin(2 * math.pi * 660 * t))
        data += struct.pack("<hh", l, r)
    with open(TONE_FILE, "wb") as f:
        f.write(data)


def get_start_round():
    if os.path.exists(STATE_FILE):
        with open(STATE_FILE) as f:
            return int(f.read().strip()) + 1
    return 1


def save_state(rnd):
    with open(STATE_FILE, "w") as f:
        f.write(str(rnd))


def get_rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except Exception:
        return None
    return None


def start_send():
    log = open("round_send.log", "a")
    p = subprocess.Popen(
        ["./aac_srt_send", PORT, "48000", "2", "96000", "150", PASSPHRASE],
        stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, env=env,
    )
    return p, log


def start_recv(outfile):
    log = open("round_recv.log", "a")
    out = open(outfile, "ab")
    p = subprocess.Popen(
        ["./aac_srt_recv", "127.0.0.1", PORT, "150", PASSPHRASE],
        stdout=out, stderr=log, env=env,
    )
    return p, log, out


def feeder_thread_target(send_proc, stop_event):
    with open(TONE_FILE, "rb") as f:
        tone = f.read()
    try:
        while not stop_event.is_set():
            send_proc.stdin.write(tone)
            send_proc.stdin.flush()
    except (BrokenPipeError, OSError):
        pass


def main():
    if not os.path.exists("./aac_srt_send") or not os.path.exists("./aac_srt_recv"):
        print("エラー: aac_srt_send / aac_srt_recv が見つかりません。")
        print("先に BUILD.md の手順でビルドしてください(このディレクトリで make)。")
        return

    ensure_tone_file()

    rss_log_is_new = not os.path.exists("round_rss.csv") or os.path.getsize("round_rss.csv") == 0
    rss_log = open("round_rss.csv", "a")
    if rss_log_is_new:
        rss_log.write("elapsed_s,round,send_rss_kb,recv_rss_kb,event\n")
    rss_log.flush()

    decoded_out = "round_decoded.raw"

    start_round = get_start_round()
    if start_round > TOTAL_ROUNDS:
        print(f"=== 既に{TOTAL_ROUNDS}ラウンド完了済みです。追加でやり直す場合は "
              f"round_state.txt 等を削除してから実行してください。 ===")
        return

    print(f"=== Round {start_round}/{TOTAL_ROUNDS} 開始 (1ラウンド{ROUND_SEC}秒) ===", flush=True)

    send_proc, send_log = start_send()
    time.sleep(1.0)
    recv_proc, recv_log, recv_out = start_recv(decoded_out)
    time.sleep(1.0)

    stop_event = threading.Event()
    feeder = threading.Thread(target=feeder_thread_target, args=(send_proc, stop_event), daemon=True)
    feeder.start()

    t0 = time.time()
    reconnect_events = []
    rnd = start_round

    round_start = time.time()
    killed_this_round = False
    while time.time() - round_start < ROUND_SEC:
        elapsed = time.time() - t0
        send_rss = get_rss_kb(send_proc.pid)
        recv_rss = get_rss_kb(recv_proc.pid)
        event = ""

        if not killed_this_round and (time.time() - round_start) >= KILL_AT:
            recv_proc.send_signal(signal.SIGKILL)
            recv_proc.wait(timeout=5)
            recv_out.close()
            recv_log.close()
            kill_time = time.time()
            recv_proc, recv_log, recv_out = start_recv(decoded_out)
            reconnect_time = time.time() - kill_time
            event = f"recv_killed_and_restarted(reconnect_wait={reconnect_time:.2f}s)"
            reconnect_events.append((rnd, reconnect_time))
            killed_this_round = True
            print(f"[round {rnd}] t={elapsed:.0f}s: {event}", flush=True)

        rss_log.write(f"{elapsed:.1f},{rnd},{send_rss},{recv_rss},{event}\n")
        rss_log.flush()
        time.sleep(SAMPLE_INTERVAL)

    print(f"=== Round {rnd}/{TOTAL_ROUNDS} 完了 (経過={time.time()-t0:.0f}秒) ===", flush=True)
    save_state(rnd)

    stop_event.set()
    try:
        send_proc.stdin.close()
    except Exception:
        pass

    time.sleep(2)
    send_proc.terminate()
    recv_proc.terminate()
    try:
        send_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        send_proc.kill()
    try:
        recv_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        recv_proc.kill()

    recv_out.close()
    recv_log.close()
    send_log.close()
    rss_log.close()

    ev_file = "round_reconnect_events.txt"
    is_new = not os.path.exists(ev_file)
    with open(ev_file, "a") as f:
        if is_new:
            f.write("round,reconnect_wait_sec\n")
        for r, t in reconnect_events:
            f.write(f"{r},{t:.2f}\n")

    if rnd >= TOTAL_ROUNDS:
        print("=== 全10ラウンド完了 ===")
        print("round_decoded.raw のFFT確認例:")
        print("  python3 -c \"")
        print("import struct,numpy as np")
        print("d=open('round_decoded.raw','rb').read()[-200000:]")
        print("n=len(d)//4")
        print("s=struct.unpack('<%dh'%(n*2), d[:n*4])")
        print("l=np.array(s[0::2],dtype=float); r=np.array(s[1::2],dtype=float)")
        print("f=np.fft.rfftfreq(len(l),1/48000)")
        print("print('L peak', f[np.argmax(np.abs(np.fft.rfft(l)))])")
        print("print('R peak', f[np.argmax(np.abs(np.fft.rfft(r)))])\"")
    print("DONE_MARKER", flush=True)


if __name__ == "__main__":
    main()
