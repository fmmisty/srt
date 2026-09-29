#!/usr/bin/env python3
"""
test_matrix.py

udp_lossy_proxy.py を介して aac_srt_send / aac_srt_recv を実行し、
様々な (loss, delay, jitter, latency_ms) の組み合わせで音声が
正しく復元されるかを検証する。

判定方法:
  - 送信したテストトーン(440Hz/660Hz)と、受信・デコードしたPCMを比較
  - 受信PCMの無音(ゼロに近い)区間の割合を検出 → 欠落・音切れの目安
  - FFTでピーク周波数がずれていないか確認
"""
import subprocess, time, os, struct, sys
import numpy as np

env = dict(os.environ)
env['LD_LIBRARY_PATH'] = '/usr/local/lib'

SR = 48000
DUR = 4.0


def gen_tone(path):
    n = int(SR * DUR)
    with open(path, 'wb') as f:
        for i in range(n):
            l = int(16000 * np.sin(2*np.pi*440.0*i/SR))
            r = int(16000 * np.sin(2*np.pi*660.0*i/SR))
            f.write(struct.pack('<hh', l, r))


def analyze(path):
    with open(path, 'rb') as f:
        data = f.read()
    n = len(data) // 4
    if n == 0:
        return {"bytes": 0, "silence_ratio": 1.0, "peak_l": 0, "peak_r": 0}
    samples = struct.unpack('<%dh' % (n*2), data[:n*4])
    l = np.array(samples[0::2], dtype=float)
    r = np.array(samples[1::2], dtype=float)

    # 無音(振幅が小さい)フレームの割合を簡易検出(欠落・音切れの目安)
    frame = 480  # 10ms分
    nframes = len(l) // frame
    silent = 0
    for i in range(nframes):
        seg = l[i*frame:(i+1)*frame]
        if np.max(np.abs(seg)) < 500:
            silent += 1
    silence_ratio = silent / nframes if nframes else 1.0

    # 中間部分でFFT
    mid = len(l) // 2
    seg_l = l[mid:mid+8000] if len(l) > mid+8000 else l[:8000]
    seg_r = r[mid:mid+8000] if len(r) > mid+8000 else r[:8000]
    freqs = np.fft.rfftfreq(len(seg_l), 1/SR)
    peak_l = freqs[np.argmax(np.abs(np.fft.rfft(seg_l)))] if len(seg_l) else 0
    peak_r = freqs[np.argmax(np.abs(np.fft.rfft(seg_r)))] if len(seg_r) else 0

    return {"bytes": len(data), "silence_ratio": silence_ratio, "peak_l": peak_l, "peak_r": peak_r}


def run_case(loss, delay_ms, jitter_ms, latency_ms, tone_path, tag):
    send_port = 9300
    proxy_port = 9301

    send_log = open(f'case_{tag}_send.log', 'w')
    recv_log = open(f'case_{tag}_recv.log', 'w')
    proxy_log = open(f'case_{tag}_proxy.log', 'w')
    recv_out = open(f'case_{tag}_decoded.raw', 'wb')

    with open(tone_path, 'rb') as f:
        pcm_data = f.read()

    send_proc = subprocess.Popen(
        ['./aac_srt_send', str(send_port), '48000', '2', '96000', str(latency_ms)],
        stdin=subprocess.PIPE, stdout=send_log, stderr=subprocess.STDOUT, env=env)
    time.sleep(1.0)

    proxy_proc = subprocess.Popen(
        ['python3', 'udp_lossy_proxy.py',
         '--listen-port', str(proxy_port),
         '--target', f'127.0.0.1:{send_port}',
         '--loss', str(loss), '--delay-ms', str(delay_ms), '--jitter-ms', str(jitter_ms),
         '--duration', str(DUR + 6)],
        stdout=proxy_log, stderr=subprocess.STDOUT, env=env)
    time.sleep(0.5)

    recv_proc = subprocess.Popen(
        ['./aac_srt_recv', '127.0.0.1', str(proxy_port), str(latency_ms)],
        stdout=recv_out, stderr=recv_log, env=env)
    time.sleep(1.0)

    send_proc.stdin.write(pcm_data)
    send_proc.stdin.close()

    try:
        send_proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        send_proc.kill()

    try:
        recv_proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        recv_proc.kill()

    try:
        proxy_proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proxy_proc.kill()

    send_log.close(); recv_log.close(); proxy_log.close(); recv_out.close()

    result = analyze(f'case_{tag}_decoded.raw')
    result['tag'] = tag
    result['loss'] = loss
    result['delay_ms'] = delay_ms
    result['jitter_ms'] = jitter_ms
    result['latency_ms'] = latency_ms
    return result


if __name__ == '__main__':
    tone_path = 'tone48.raw'
    if not os.path.exists(tone_path):
        gen_tone(tone_path)

    # (loss, delay_ms, jitter_ms, latency_ms, tag)
    cases = [
        (0.00, 0,   0,  150, 'baseline_150'),
        (0.01, 0,   0,  150, 'loss1pct_150'),
        (0.01, 0,   0,  300, 'loss1pct_300'),
        (0.03, 0,   0,  150, 'loss3pct_150'),
        (0.03, 0,   0,  300, 'loss3pct_300'),
        (0.01, 100, 20, 150, 'loss1pct_delay100_150'),
        (0.01, 100, 20, 300, 'loss1pct_delay100_300'),
        (0.05, 200, 50, 300, 'loss5pct_delay200_300'),
        (0.05, 200, 50, 500, 'loss5pct_delay200_500'),
    ]

    results = []
    for loss, delay_ms, jitter_ms, latency_ms, tag in cases:
        print(f'=== running {tag} ===', flush=True)
        r = run_case(loss, delay_ms, jitter_ms, latency_ms, tone_path, tag)
        results.append(r)
        print(r, flush=True)

    print('\n\n=== SUMMARY ===')
    print(f"{'tag':28s} {'loss%':>6s} {'delay':>6s} {'jit':>5s} {'lat':>5s} {'bytes':>8s} {'silence%':>9s} {'peakL':>7s} {'peakR':>7s}")
    for r in results:
        print(f"{r['tag']:28s} {r['loss']*100:6.1f} {r['delay_ms']:6.0f} {r['jitter_ms']:5.0f} "
              f"{r['latency_ms']:5.0f} {r['bytes']:8d} {r['silence_ratio']*100:9.2f} "
              f"{r['peak_l']:7.1f} {r['peak_r']:7.1f}")
