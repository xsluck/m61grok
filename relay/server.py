#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
m61-tunnel relay v2 —— ngrok 式反向隧道中继，跑在有公网 IP 的 VPS 上。

用法（推荐单端口模式，板子出站只需要能到 7000 一个端口）：
  python3 server.py --token <TOKEN> --single-port 7000

v3 SOCKS5：
  单端口模式下 SOCKS5 与控制/数据/状态页共用端口（首字节 0x05 自动识别），
  认证用隧道 token（RFC1929 用户名任意/密码=token），连接内网任意 IP:端口。
  新消息: OPENX <cid> <host> <port> / AUTHX <token> <cid> <host> <port>

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
        self.socks_enabled = True        # 板子经 HELLO ... SOCKS=0/1 控制，管理页开关
        self.socks_pass = None           # 板子经 HELLO ... SOCKS_PASS=xxx 下发；None=回退用 token
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

    def _privnet_only(self, host: str) -> bool:
        """SOCKS 目标白名单：默认只放行私网地址（防被当开放代理滥用）。"""
        try:
            import ipaddress
            ip = ipaddress.ip_address(host)
            return ip.is_private or ip.is_loopback
        except ValueError:
            return False

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
        """单端口(7000)总入口：0x05=SOCKS5，HELLO=控制，AUTH=数据，其他=说明页。"""
        try:
            first = await asyncio.wait_for(reader.readexactly(1), 15)
        except Exception:
            self._close_writer(writer)
            return
        if first == b"\x05":
            await self._socks5_flow(reader, writer)
            return
        try:
            raw, line = await self._read_first_line(reader, 15)
        except Exception:
            self._close_writer(writer)
            return
        if raw is None:
            line = None
        else:
            line = (first + raw).decode("utf-8", "replace").rstrip("\r\n")
        if not line:
            self._close_writer(writer)
            return
        if line.startswith("HELLO"):
            await self._control_flow(reader, writer, line)
        elif line.startswith("AUTH") or line.startswith("AUTHX"):
            await self._data_flow(reader, writer, line)
        else:
            await self._default_http_flow(reader, writer, first + (raw or b""))

    async def _dynamic_forward(self, reader, writer, host, port,
                               prefix=b"", socks_reply=False):
        """让板子连任意目标（OPENX/AUTHX 动态通道）并双向转发。
        返回 True=转发已完成后关闭，False=建立失败（调用方自行善后）。"""
        cid = self.next_cid + 1000000
        self.next_cid += 1
        fut = asyncio.get_event_loop().create_future()
        self.pending[(-1, cid)] = fut
        try:
            self.ctrl_writer.write(("OPENX %d %s %d\n" % (cid, host, port)).encode())
            await self.ctrl_writer.drain()
        except Exception as e:
            log.exception("dyn forward send failed: %s", e)
            self.pending.pop((-1, cid), None)
            self._drop_control()
            return False
        try:
            br, bw = await asyncio.wait_for(fut, 15)
        except Exception:
            self.pending.pop((-1, cid), None)
            return False

        if socks_reply:
            writer.write(b"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00")
            await writer.drain()
        self.stats["tunnel_ok"] += 1
        log.info("dyn %s:%s cid=%d%s", host, port, cid,
                 " (socks)" if socks_reply else " (default http)")

        async def pump(src, dst, stat_key):
            try:
                while True:
                    data = await asyncio.wait_for(src.read(8192), 600)
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
                return True
        await asyncio.gather(
            pump(br, writer, "bytes_board_to_visitor"),
            pump(reader, bw, "bytes_visitor_to_board"),
        )
        self._close_writer(writer)
        self._close_writer(bw)
        return True

    async def _socks5_flow(self, reader, writer):
        """SOCKS5 (RFC1928 + RFC1929 密码认证)。认证=token；仅放行私网目标。"""
        peer = writer.get_extra_info("peername")
        if not self.socks_enabled:
            try:
                writer.write(b"\x05\xff")  # 无可用认证方式 = 服务拒绝
                await writer.drain()
            except Exception:
                pass
            self._close_writer(writer)
            log.info("socks5 %s: disabled by board config", peer)
            return
        try:
            # 握手（首字节 0x05 已由 handle_client 消费，这里从 NMETHODS 开始）
            nmethods = (await asyncio.wait_for(reader.readexactly(1), 10))[0]
            methods = await asyncio.wait_for(reader.readexactly(nmethods), 10)
            if 2 not in methods:  # 要求用户名/密码认证
                writer.write(b"\x05\xff")
                await writer.drain()
                self._close_writer(writer)
                return
            writer.write(b"\x05\x02")
            await writer.drain()
            # RFC1929 子协商
            sub = await asyncio.wait_for(reader.readexactly(2), 10)
            ulen = sub[1]
            user = (await asyncio.wait_for(reader.readexactly(ulen), 10)).decode("utf-8", "replace")
            plen = (await asyncio.wait_for(reader.readexactly(1), 10))[0]
            password = (await asyncio.wait_for(reader.readexactly(plen), 10)).decode("utf-8", "replace")
            expect = self.socks_pass if self.socks_pass is not None else self.token
            if not hmac.compare_digest(password, expect):
                writer.write(b"\x01\x01")
                await writer.drain()
                self._close_writer(writer)
                log.warning("socks5 %s: auth failed (user=%r)", peer, user[:32])
                return
            writer.write(b"\x01\x00")
            await writer.drain()
            # CONNECT
            hdr = await asyncio.wait_for(reader.readexactly(4), 10)
            if hdr[1] != 1:  # 仅支持 CONNECT
                writer.write(b"\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00")
                self._close_writer(writer)
                return
            atyp = hdr[3]
            if atyp == 1:
                raw_addr = await asyncio.wait_for(reader.readexactly(4), 10)
                host = ".".join(str(b) for b in raw_addr)
            elif atyp == 3:
                dlen = (await asyncio.wait_for(reader.readexactly(1), 10))[0]
                host = (await asyncio.wait_for(reader.readexactly(dlen), 10)).decode("utf-8", "replace")
            else:
                writer.write(b"\x05\x08\x00\x01\x00\x00\x00\x00\x00\x00")
                self._close_writer(writer)
                return
            praw = await asyncio.wait_for(reader.readexactly(2), 10)
            port = (praw[0] << 8) | praw[1]

            if not self._privnet_only(host):
                log.info("socks5 %s: reject public target %s:%d", peer, host, port)
                writer.write(b"\x05\x02\x00\x01\x00\x00\x00\x00\x00\x00")
                await writer.drain()
                self._close_writer(writer)
                return

            if self.ctrl_writer is None:
                writer.write(b"\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00")
                await writer.drain()
                self._close_writer(writer)
                return

            ok = await self._dynamic_forward(reader, writer, host, port,
                                             socks_reply=True)
            if not ok:
                writer.write(b"\x05\x04\x00\x01\x00\x00\x00\x00\x00\x00")
                await writer.drain()
        except (asyncio.IncompleteReadError, asyncio.TimeoutError):
            pass
        except Exception as e:
            log.debug("socks5 %s ended: %s", peer, e)
        finally:
            self._close_writer(writer)

    async def _default_http_flow(self, reader, writer, prefix: bytes):
        """7000 端口收到普通 HTTP 请求：
        板子在线且映射表有 local 条目 → 转发到板载管理页（默认绑定）；
        否则返回中继状态页（兼做诊断）。"""
        local_tid = None
        for tid, t in self.tunnels.items():
            if t["target"].split(":")[0] == "local":
                local_tid = tid
                break
        if self.ctrl_writer is not None:
            # 默认绑定：动态转发到板载管理页，不依赖映射表条目
            ok = await self._dynamic_forward(reader, writer, "local", 80, prefix)
            if ok:
                return
            body = b"board unreachable\n"
        body = ("m61-tunnel relay online=%s\n"
                "visitor ports: %s\n"
                "port %d: control/data%s + mgmt-page(default)\n" %
                (self.ctrl_writer is not None,
                 ", ".join(str(t["vport"]) + "->" + t["target"]
                           for t in sorted(self.tunnels.values(),
                                           key=lambda x: x["vport"])) or "(none)",
                 self.public_port,
                 "/socks5(on)" if self.socks_enabled else "/socks5(off)")).encode()
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
        extra = parts[3] if len(parts) > 3 else ""
        # SOCKS=n / SOCKS_PASS=xxx 字段独立摘取（可出现在 TUNNELS 段内/后）
        socks_val = None
        if "SOCKS_PASS=" in extra:
            idx = extra.find("SOCKS_PASS=")
            seg = extra[idx + 11:].split(None, 1)
            tail = seg[0] if seg else ""   # 行尾空值不崩
            self.socks_pass = tail if tail else None
            extra = (extra[:idx] + " " + extra[idx + 11 + len(tail):]).strip()
        if "SOCKS=" in extra:
            idx = extra.find("SOCKS=")
            seg = extra[idx + 6:].split(None, 1)
            tail = seg[0] if seg else ""
            socks_val = tail
            extra = (extra[:idx] + " " + extra[idx + 6 + len(tail):]).strip()
        if socks_val is not None:
            self.socks_enabled = socks_val in ("1", "true")
        spec = extra[8:].strip() if extra.startswith("TUNNELS") else ""

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
        log.info("board online: %s tunnel_id=%s socks=%s pass=%s", peer, tunnel_id,
                 "on" if self.socks_enabled else "off",
                 "custom" if self.socks_pass is not None else "token")

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
                elif line.startswith("SETTOKEN "):
                    new_tok = line[9:].strip()
                    if len(new_tok) >= 6 and " " not in new_tok:
                        old_tok = self.token
                        self.token = new_tok
                        ok = True
                        try:  # 持久化：重启后仍是新 token（板子是真相源）
                            import os
                            tf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "token")
                            with open(tf, "w") as f:
                                f.write(new_tok)
                            os.chmod(tf, 0o600)
                        except Exception as e:
                            log.warning("SETTOKEN: persist failed: %s", e)
                            ok = False
                            self.token = old_tok  # 写盘失败则回滚，双端一致
                        writer.write(b"OK\n" if ok else b"ERR persist failed\n")
                        await writer.drain()
                        if ok:
                            log.info("token updated by board (persisted)")
                    else:
                        writer.write(b"ERR bad token (min 6, no spaces)\n")
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
        # v3: AUTHX token cid | v2: AUTH token tid cid | v1: AUTH token cid
        if parts[0] == "AUTHX" and len(parts) == 5:
            tid, cid_s = "-1", parts[2]  # AUTHX token cid host port（目标由板子连）
        elif len(parts) == 4:
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
    import os
    _tf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "token")
    if os.path.exists(_tf):
        _saved = open(_tf).read().strip()
        if _saved and _saved != token:
            log.info("using persisted token from %s (board-updated)", _tf)
            token = _saved

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
