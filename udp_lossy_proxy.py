#!/usr/bin/env python3
"""
udp_lossy_proxy.py

tc netem が使えない検証環境向けの代替ツール。
2点間のUDPパケットを中継しつつ、指定した確率でパケットをドロップし、
指定した遅延(+ジッタ)を加える。SRTは単一UDPソケットで双方向通信する
ため、双方向とも同じ条件を適用する。

使い方:
  python3 udp_lossy_proxy.py --listen-port 9001 --target 127.0.0.1:9000 \
      --loss 0.01 --delay-ms 100 --jitter-ms 20
"""
import argparse
import asyncio
import random


class LossyProxy(asyncio.DatagramProtocol):
    def __init__(self, target_addr, loss, delay_ms, jitter_ms, loop):
        self.target_addr = target_addr
        self.loss = loss
        self.delay_ms = delay_ms
        self.jitter_ms = jitter_ms
        self.loop = loop
        self.transport = None
        self.client_addr = None  # 受信側(実際にproxyに接続してきたSRTソケット)のアドレス
        self.stats = {"total": 0, "dropped": 0}

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        self.stats["total"] += 1

        if addr == self.target_addr:
            # target(送信側SRTソケット) -> client(受信側) 方向
            dest = self.client_addr
        else:
            # client(受信側) -> target(送信側) 方向
            self.client_addr = addr
            dest = self.target_addr

        if dest is None:
            return

        if random.random() < self.loss:
            self.stats["dropped"] += 1
            return

        delay = max(0.0, (self.delay_ms + random.uniform(-self.jitter_ms, self.jitter_ms)) / 1000.0)
        if delay > 0:
            self.loop.call_later(delay, self._send, data, dest)
        else:
            self._send(data, dest)

    def _send(self, data, dest):
        try:
            self.transport.sendto(data, dest)
        except Exception:
            pass


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen-port", type=int, required=True)
    ap.add_argument("--target", required=True, help="host:port")
    ap.add_argument("--loss", type=float, default=0.0, help="0.0-1.0")
    ap.add_argument("--delay-ms", type=float, default=0.0)
    ap.add_argument("--jitter-ms", type=float, default=0.0)
    ap.add_argument("--duration", type=float, default=15.0, help="seconds to run")
    args = ap.parse_args()

    host, port = args.target.split(":")
    target_addr = (host, int(port))

    loop = asyncio.get_running_loop()
    transport, protocol = await loop.create_datagram_endpoint(
        lambda: LossyProxy(target_addr, args.loss, args.delay_ms, args.jitter_ms, loop),
        local_addr=("0.0.0.0", args.listen_port),
    )

    print(f"[proxy] listen=:{args.listen_port} target={args.target} "
          f"loss={args.loss*100:.2f}% delay={args.delay_ms}ms jitter={args.jitter_ms}ms", flush=True)

    await asyncio.sleep(args.duration)

    st = protocol.stats
    rate = (st["dropped"] / st["total"] * 100) if st["total"] else 0.0
    print(f"[proxy] 統計: total={st['total']} dropped={st['dropped']} ({rate:.2f}%)", flush=True)
    transport.close()


if __name__ == "__main__":
    asyncio.run(main())
