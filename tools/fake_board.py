#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
m61-tunnel 客户端 v2（PC 版，纯标准库）。

三种用法：
  1) 无硬件验证 VPS 中继（内置演示页）
     python3 fake_board.py --relay-host 1.2.3.4 --token xxx
  2) 隧道到本机单个服务（v1 兼容）
     python3 fake_board.py --relay-host 1.2.3.4 --token xxx --target 127.0.0.1:8000
  3) v2 多目标（和板子固件同款协议）
     python3 fake_board.py --relay-host 1.2.3.4 --token xxx \
        --tunnels "21114=127.0.0.1:8080,21115=192.168.0.51:30001"

v2 协议：HELLO ... TUNNELS vport=host:port,... / OPEN tid cid / AUTH token tid cid
"""

import argparse
import asyncio
import logging

log = logging.getLogger("board")

DEMO_PAGE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>m61-tunnel</title></head>
<body style="font-family: sans-serif; margin: 3em;">
<h1>OK fake_board v2 online</h1>
<p>PC 客户端演示页。多目标模式请访问映射出来的端口。</p>
</body></html>"""


async def readline(reader, timeout=10.0):
    raw = await asyncio.wait_for(reader.readline(), timeout)
    if not raw:
        return None
    return raw.decode("utf-8", "replace").rstrip("\r\n")


class FakeBoard:
    def __init__(self, args):
        self.args = args
        self.ctrl_reader = None
        self.ctrl_writer = None
        # tunnels: tid -> (vport, host, port)；单 target 时等价 tid=0
        self.tunnels = []
        if args.tunnels:
            for item in args.tunnels.split(","):
                item = item.strip()
                if not item:
                    continue
                vport_s, target = item.split("=", 1)
                host, _, port = target.rpartition(":")
                self.tunnels.append((int(vport_s), host or "127.0.0.1", int(port)))
        elif args.target:
            thost, tport = args.target
            self.tunnels.append((args.public_port, thost, tport))

    # ---------- 控制通道 ----------

    async def run_forever(self):
        backoff = 2
        while True:
            try:
                await self.connect_control()
                backoff = 2
                await self.control_loop()
            except Exception as e:
                log.warning("control channel lost: %s, retry in %ss", e, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 60)

    async def connect_control(self):
        log.info("connecting control channel %s:%d",
                 self.args.relay_host, self.args.ctrl_port)
        self.ctrl_reader, self.ctrl_writer = await asyncio.open_connection(
            self.args.relay_host, self.args.ctrl_port)
        spec = ",".join("%d=%s:%d" % t for t in self.tunnels)
        hello = ("HELLO %s %s TUNNELS %s SOCKS=%d\n"
                 % (self.args.token, self.args.tunnel_id, spec,
                    getattr(self.args, "socks", 1)))
        self.ctrl_writer.write(hello.encode())
        await self.ctrl_writer.drain()
        line = await readline(self.ctrl_reader, 10)
        if not line or not line.startswith("OK"):
            raise ConnectionError("handshake failed: %r" % line)
        log.info("control OK (%s). tunnels=%s", line, self.tunnels or "(demo page)")

    async def control_loop(self):
        while True:
            line = await readline(self.ctrl_reader, 60)
            if line is None:
                raise ConnectionError("control EOF")
            if line == "PING":
                self.ctrl_writer.write(b"PONG\n")
                await self.ctrl_writer.drain()
            elif line.startswith("OPENX"):  # v3 SOCKS5: OPENX cid host port
                parts = line.split()
                if len(parts) == 4:
                    asyncio.ensure_future(self.data_channel(
                        int(parts[1]), -1, parts[2], int(parts[3])))
            elif line.startswith("OPEN"):
                parts = line.split()          # v2: OPEN tid cid / v1: OPEN cid
                if len(parts) == 3:
                    tid, cid = int(parts[1]), int(parts[2])
                else:
                    tid, cid = 0, int(parts[1])
                asyncio.ensure_future(self.data_channel(cid, tid))
            elif line:
                log.debug("ctrl ignored: %r", line)

    # ---------- 数据通道 ----------

    async def data_channel(self, cid, tid, dyn_host=None, dyn_port=0):
        w = None
        try:
            r, w = await asyncio.wait_for(
                asyncio.open_connection(self.args.relay_host, self.args.data_port), 10)
            if tid < 0:
                w.write(("AUTHX %s %d %s %d\n" % (self.args.token, cid, dyn_host, dyn_port)).encode())
            else:
                w.write(("AUTH %s %d %d\n" % (self.args.token, tid, cid)).encode())
            await w.drain()
            line = await readline(r, 10)
            if not line or not line.startswith("OK"):
                log.warning("cid %d auth failed: %r", cid, line)
                return
            log.info("cid %d (tid %d) data channel up", cid, tid)

            thost = tport = None
            if tid < 0:
                thost, tport = dyn_host, dyn_port
            elif tid < len(self.tunnels):
                _, thost, tport = self.tunnels[tid]
            if thost == "local":
                thost = None  # local = 板载管理页 → 走内置演示页
            if thost:
                tr, tw = await asyncio.open_connection(thost, tport)
            else:
                tr, tw = None, None

            async def demo_responder():
                head = b""
                while b"\r\n\r\n" not in head and len(head) < 8192:
                    chunk = await r.read(1024)
                    if not chunk:
                        break
                    head += chunk
                body = DEMO_PAGE.encode()
                w.write(("HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                         "Content-Length: %d\r\nConnection: close\r\n\r\n"
                         % len(body)).encode() + body)
                await w.drain()

            async def pump(src, dst):
                try:
                    while True:
                        data = await src.read(8192)
                        if not data:
                            break
                        dst.write(data)
                        await dst.drain()
                except Exception:
                    pass
                finally:
                    try:
                        if dst.can_write_eof():
                            dst.write_eof()
                    except Exception:
                        pass

            if tr is None:
                await demo_responder()
            else:
                await asyncio.gather(pump(r, tw), pump(tr, w))
                tw.close()
        except Exception as e:
            log.warning("cid %d failed: %s", cid, e)
        finally:
            if w is not None:
                try:
                    w.close()
                except Exception:
                    pass


async def ping_loop(board):
    while True:
        await asyncio.sleep(15)
        try:
            if board.ctrl_writer is not None:
                board.ctrl_writer.write(b"PING\n")
                await board.ctrl_writer.drain()
        except Exception:
            pass


async def main():
    ap = argparse.ArgumentParser(description="m61-tunnel PC client v2")
    ap.add_argument("--relay-host", required=True)
    ap.add_argument("--ctrl-port", type=int, default=7000)
    ap.add_argument("--data-port", type=int, default=7000,
                    help="默认 7000=单端口模式；中继若用三端口模式则改 7001")
    ap.add_argument("--token", required=True)
    ap.add_argument("--tunnel-id", default="fakeboard")
    ap.add_argument("--target", default=None, metavar="HOST:PORT",
                    help="v1 单目标：要隧道到的本地服务")
    ap.add_argument("--public-port", type=int, default=7002,
                    help="v1 单目标对应的公网访客端口（默认 7002）")
    ap.add_argument("--socks", type=int, default=1, choices=[0, 1],
                    help="SOCKS5 开关（随 HELLO 下发中继）")
    ap.add_argument("--tunnels", default=None,
                    help="v2 多目标: \"21114=127.0.0.1:8080,21115=192.168.0.51:30001\"")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    if args.target:
        if ":" in args.target:
            thost, tport = args.target.rsplit(":", 1)
        else:
            thost, tport = "127.0.0.1", args.target
        args.target = (thost, int(tport))

    board = FakeBoard(args)
    asyncio.ensure_future(ping_loop(board))
    await board.run_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
