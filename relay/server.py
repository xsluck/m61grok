#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
m61-tunnel relay v2 —— ngrok 式反向隧道中继，跑在有公网 IP 的 VPS 上。

用法（推荐单端口模式，板子出站只需要能到 7000 一个端口）：
  python3 server.py --token <TOKEN> --single-port 7000

v2 多目标：
  板子 HELLO 时带上映射表: HELLO <token> <id> TUNNELS 21114=192.168.0.127:8080,21116=local:80
  中继为每个公网访客端口（21114...）开监听；访客连哪个端口就转发到哪个目标。
  映射表由板端管理页在线修改（保存后板子重连，中继自动更新监听端口）。

协议（与 board/m61_tunnel/tunnel.c、tools/fake_board.py 对应）：
  控制(板→中继): HELLO <token> <id> [TUNNELS vport=host:port,...]  → OK <port> / ERR
                  PING → PONG（15s 心跳，60s 无数据断开）
  中继→板:       OPEN <tid> <cid>          ← tid=目标编号（表内序号）
  数据(板→中继): AUTH <token> <tid> <cid>  → OK，此后裸字节双向透传
  兼容 v1:       OPEN <cid> / AUTH <token> <cid>（单目标，tid=0）

只用 Python 标准库。Python >= 3.8。
"""

import argparse
import asyncio
import hmac
import logging
import secrets
import ssl
import time

log = logging.getLogger("relay")


class Relay:
    def __init__(self, token: str, public_host: str, public_port: int):
        self.token = token
        self.public_host = public_host
        self.public_port = public_port
        self.ctrl_reader = None          # 板子控制通道
        self.ctrl_writer = None
        self.ctrl_peer = None
        self.pending = {}                # (tid, cid) -> Future[(reader, writer)]
        self.next_cid = 1
        self.tunnels = {}                # tid -> {"vport": int, "target": str, "server": asyncio.Server}
        self.board_last_seen = 0.0
        self.stats = {"visitor_total": 0, "tunnel_ok": 0, "tunnel_fail": 0,
                      "bytes_board_to_visitor": 0, "bytes_visitor_to_board": 0,
                      "started_at": time.time()}

    # ---------- 工具 ----------

    def token_ok(self, t: str) -> bool:
        return hmac.compare_digest(t, self.token)

    async def _readline(self, reader: asyncio.StreamReader, timeout: float = 10.0):
        try:
            raw = await asyncio.wait_for(reader.readline(), timeout)
        except asyncio.TimeoutError:
            raise TimeoutError("line timeout")
        if not raw:
            return None
        return raw.decode("utf-8", "replace").rstrip("\r\n")

    async def _read_first_line(self, reader: asyncio.StreamReader, timeout: float = 10.0):
        raw = bytearray()
        try:
            while len(raw) < 1024:
                c = await asyncio.wait_for(reader.read(1), timeout)
                if not c:
                    return (None, None) if not raw else (bytes(raw), None)
                raw += c
                if c == b"\n":
                    break
        except asyncio.TimeoutError:
            raise TimeoutError("first line timeout")
        return bytes(raw), raw.decode("utf-8", "replace").rstrip("\r\n")

    def _close_writer(self, writer):
        try:
            writer.close()
        except Exception:
            pass

    def _try_write_eof(self, writer):
        try:
            if writer.can_write_eof():
                writer.write_eof()
        except Exception:
            pass

    # ---------- 多目标监听管理 ----------

    async def apply_tunnels(self, spec: str):
        """按板子发来的 TUNNELS 表差异更新访客端口监听。"""
        want = {}
        for i, item in enumerate(spec.split(",")):
            item = item.strip()
            if not item or "=" not in item:
                continue
            vport_s, target = item.split("=", 1)
            try:
                vport = int(vport_s)
            except ValueError:
                continue
            if not (1 <= vport <= 65535) or vport in (22, 7000):
                log.warning("skip bad visitor port %d", vport)
                continue
            want[i] = {"vport": vport, "target": target, "server": None}

        # 关掉不在新表里的
        for tid, t in list(self.tunnels.items()):
            if tid not in want or want[tid]["vport"] != t["vport"]:
                log.info("close visitor port %d (tid %d)", t["vport"], tid)
                t["server"].close()
                del self.tunnels[tid]

        # 起新的
        loop = asyncio.get_event_loop()
        for tid, t in want.items():
            existing = self.tunnels.get(tid)
            if existing and existing["vport"] == t["vport"]:
                self.tunnels[tid]["target"] = t["target"]
                continue
            server = await asyncio.start_server(
                lambda r, w, _tid=tid: self.handle_mapped_visitor(r, w, _tid),
                host="0.0.0.0", port=t["vport"])
            self.tunnels[tid] = {"vport": t["vport"], "target": t["target"], "server": server}
            log.info("visitor port %d -> %s (tid %d)", t["vport"], t["target"], tid)

    # ---------- 连接处理器 ----------

    async def handle_client(self, reader, writer):
        """单端口(7000)总入口：HELLO=控制，AUTH=数据，其他=说明页。"""
        try:
            raw, line = await self._read_first_line(reader, 15)
        except Exception:
            self._close_writer(writer)
            return
        if not line:
            self._close_writer(writer)
            return
        if line.startswith("HELLO"):
            await self._control_flow(reader, writer, line)
        elif line.startswith("AUTH"):
            await self._data_flow(reader, writer, line)
        else:
            await self._status_flow(writer)

    async def _status_flow(self, writer):
        """7000 端口收到非协议流量（v2 模式下访客请走映射端口）。"""
        body = ("m61-tunnel relay v2 online=%s\n"
                "visitor ports: %s\n"
                "tunnel/control port: %d\n" %
                (self.ctrl_writer is not None,
                 ", ".join(str(t["vport"]) + "->" + t["target"]
                           for t in sorted(self.tunnels.values(),
                                           key=lambda x: x["vport"])) or "(none)",
                 self.public_port)).encode()
        head = ("HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body)).encode()
        try:
            writer.write(head + body)
            await writer.drain()
        except Exception:
            pass
        self._close_writer(writer)

    async def handle_mapped_visitor(self, reader, writer, tid):
        """v2 映射端口的访客：直接按 tid 配对。"""
        await self._visitor_flow(reader, writer, b"", tid)

    async def handle_visitor(self, reader, writer):
        """v1 三端口模式的访客口。"""
        await self._visitor_flow(reader, writer, b"", 0)

    async def _control_flow(self, reader, writer, line):
        peer = writer.get_extra_info("peername")
        parts = line.split(None, 3)
        if len(parts) < 3 or parts[0] != "HELLO" or not self.token_ok(parts[1]):
            writer.write(b"ERR bad token\n")
            try:
                await writer.drain()
            except Exception:
                pass
            self._close_writer(writer)
            log.warning("control %s: bad handshake: %r", peer, line[:80])
            return
        tunnel_id = parts[2][:64]
        spec = parts[3] if len(parts) > 3 else ""
        if spec.startswith("TUNNELS "):
            spec = spec[8:].strip()
        else:
            spec = ""

        if self.ctrl_writer is not None:  # 新连接顶掉旧的
            log.info("new control %s kicks old control %s", peer, self.ctrl_peer)
            try:
                self.ctrl_writer.close()
            except Exception:
                pass
        self.ctrl_reader, self.ctrl_writer = reader, writer
        self.ctrl_peer = peer
        self.board_last_seen = time.time()
        writer.write("OK %d\n".encode() % self.public_port)
        try:
            await writer.drain()
        except Exception:
            self._drop_control()
            return
        log.info("board online: %s tunnel_id=%s", peer, tunnel_id)

        if spec:
            try:
                await self.apply_tunnels(spec)
            except Exception as e:
                log.warning("apply_tunnels failed: %s", e)

        try:
            while True:
                line = await self._readline(reader, 60)
                if line is None:
                    break
                self.board_last_seen = time.time()
                if line == "PING":
                    writer.write(b"PONG\n")
                    await writer.drain()
                elif line:
                    log.debug("control msg ignored: %r", line[:80])
        except Exception as e:
            log.info("control %s ended: %s", peer, e)
        finally:
            if self.ctrl_writer is writer:
                self._drop_control()

    def _drop_control(self):
        self.ctrl_reader = None
        w, self.ctrl_writer = self.ctrl_writer, None
        if w is not None:
            try:
                w.close()
            except Exception:
                pass
        for key, fut in list(self.pending.items()):
            if not fut.done():
                fut.set_exception(ConnectionError("board control dropped"))
        self.pending.clear()
        log.info("board offline")

    async def _data_flow(self, reader, writer, line):
        peer = writer.get_extra_info("peername")
        parts = line.split()
        # v2: AUTH token tid cid / v1: AUTH token cid
        if len(parts) == 4:
            tid, cid_s = parts[2], parts[3]
        elif len(parts) == 3:
            tid, cid_s = "0", parts[2]
        else:
            tid = cid_s = None
        if not tid or not self.token_ok(parts[1]):
            writer.write(b"ERR bad token\n")
            try:
                await writer.drain()
            except Exception:
                pass
            self._close_writer(writer)
            return
        try:
            key = (int(tid), int(cid_s))
        except (TypeError, ValueError):
            self._close_writer(writer)
            return

        fut = self.pending.pop(key, None)
        if fut is None or fut.done():
            writer.write(b"ERR unknown cid\n")
            try:
                await writer.drain()
            except Exception:
                pass
            self._close_writer(writer)
            log.warning("data %s: %s not pending", peer, key)
            return

        writer.write(b"OK\n")
        try:
            await writer.drain()
        except Exception:
            self._close_writer(writer)
            fut.set_exception(ConnectionError("data channel broke before OK"))
            return
        log.debug("data channel %s from %s", key, peer)
        fut.set_result((reader, writer))

    async def _visitor_flow(self, reader, writer, prefix: bytes, tid: int = 0):
        peer = writer.get_extra_info("peername")
        self.stats["visitor_total"] += 1

        def goodbye(msg: str):
            body = msg.encode()
            head = ("HTTP/1.0 502 Bad Gateway\r\nContent-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body)).encode()
            try:
                writer.write(head + body)
            except Exception:
                pass

        if self.ctrl_writer is None:
            goodbye("board offline\n")
            self._close_writer(writer)
            log.info("visitor %s rejected: board offline", peer)
            return

        cid = self.next_cid
        self.next_cid += 1
        fut = asyncio.get_event_loop().create_future()
        self.pending[(tid, cid)] = fut
        try:
            self.ctrl_writer.write("OPEN %d %d\n".encode() % (tid, cid))
            await self.ctrl_writer.drain()
        except Exception:
            self.pending.pop((tid, cid), None)
            goodbye("board control dropped\n")
            self._close_writer(writer)
            self._drop_control()
            return

        try:
            br, bw = await asyncio.wait_for(fut, 15)
        except asyncio.TimeoutError:
            self.pending.pop((tid, cid), None)
            self.stats["tunnel_fail"] += 1
            goodbye("board did not open data channel in 15s\n")
            self._close_writer(writer)
            log.warning("visitor %s: board too slow for tid=%d cid=%d", peer, tid, cid)
            return
        except Exception:
            self.pending.pop((tid, cid), None)
            self.stats["tunnel_fail"] += 1
            goodbye("board control dropped\n")
            self._close_writer(writer)
            return

        self.stats["tunnel_ok"] += 1
        target = self.tunnels.get(tid, {}).get("target", "?")
        log.info("tunnel %s <-> tid=%d(%s) cid=%d", peer, tid, target, cid)

        async def pump(src, dst, stat_key):
            try:
                while True:
                    data = await asyncio.wait_for(src.read(8192), 600)  # 10min空闲断开：防死连接协程泄漏
                    if not data:
                        break
                    dst.write(data)
                    self.stats[stat_key] += len(data)
                    await dst.drain()
            except Exception:
                pass
            finally:
                self._try_write_eof(dst)

        if prefix:
            try:
                bw.write(prefix)
                self.stats["bytes_visitor_to_board"] += len(prefix)
                await bw.drain()
            except Exception:
                self._close_writer(writer)
                self._close_writer(bw)
                return

        await asyncio.gather(
            pump(br, writer, "bytes_board_to_visitor"),
            pump(reader, bw, "bytes_visitor_to_board"),
        )
        self._close_writer(writer)
        self._close_writer(bw)
        log.info("tunnel tid=%d cid=%d closed", tid, cid)

    async def stats_task(self):
        while True:
            await asyncio.sleep(60)
            s = self.stats
            log.info("stats: online=%s tunnels=%s visitors=%d ok=%d fail=%d v2b=%dKB b2v=%dKB",
                     self.ctrl_writer is not None,
                     {t["vport"]: t["target"] for t in self.tunnels.values()},
                     s["visitor_total"], s["tunnel_ok"], s["tunnel_fail"],
                     s["bytes_visitor_to_board"] // 1024,
                     s["bytes_board_to_visitor"] // 1024)


def make_ssl_ctx(certfile, keyfile):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile, keyfile)
    return ctx


async def main():
    ap = argparse.ArgumentParser(description="m61-tunnel relay server v2")
    ap.add_argument("--token", default=None,
                    help="共享密钥（强烈建议 32+ 随机字符）。不给则随机生成并打印。")
    ap.add_argument("--listen", default="0.0.0.0:7002", metavar="HOST:PORT",
                    help="v1 三端口模式的公网访客地址 (默认 0.0.0.0:7002)")
    ap.add_argument("--ctrl-port", type=int, default=7000)
    ap.add_argument("--data-port", type=int, default=7001)
    ap.add_argument("--single-port", type=int, default=None,
                    help="单端口模式：控制+数据走这个口（推荐 7000）；"
                         "v2 多目标的访客端口由板子的映射表动态决定")
    ap.add_argument("--tls-cert", default=None)
    ap.add_argument("--tls-key", default=None)
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    token = args.token or secrets.token_hex(16)

    host, _, port = args.listen.rpartition(":")
    host = host or "0.0.0.0"

    ssl_ctx = None
    if args.tls_cert and args.tls_key:
        ssl_ctx = make_ssl_ctx(args.tls_cert, args.tls_key)

    if args.single_port:
        relay = Relay(token, host, args.single_port)
        server = await asyncio.start_server(
            relay.handle_client, host="0.0.0.0", port=args.single_port, ssl=ssl_ctx)
        log.info("m61-tunnel relay v2 up (single-port %d). "
                 "visitor ports come from board's tunnel map.", args.single_port)
        log.info("TOKEN = %s", token)
        asyncio.ensure_future(relay.stats_task())
        async with server:
            await server.serve_forever()
        return

    relay = Relay(token, host, int(port))
    server_ctrl = await asyncio.start_server(
        relay.handle_client, host="0.0.0.0", port=args.ctrl_port, ssl=ssl_ctx)
    server_pub = await asyncio.start_server(
        relay.handle_visitor, host=host, port=int(port))
    log.info("m61-tunnel relay v2 up. ctrl=%d visitor=%s", args.ctrl_port, args.listen)
    log.info("TOKEN = %s", token)
    asyncio.ensure_future(relay.stats_task())
    async with server_ctrl, server_pub:
        await asyncio.gather(server_ctrl.serve_forever(),
                             server_pub.serve_forever())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
