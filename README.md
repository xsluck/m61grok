# m61grok

**A tiny ngrok for RISC-V MCUs** — 用一块几十块的 [AI-M61-32S](https://docs.zephyrproject.org/latest/boards/aithinker/ai_m61_32s_kit/doc/index.html)（博流 BL618，RISC-V 320MHz）开发板，把家里/办公室局域网的服务发布到公网。不需要云厂商、不需要付费隧道服务，一台最便宜的 VPS 就够。

```
                 任何访客（手机 / 外网同事）
                        │  http://<vps-ip>:21114/
                        ▼
        ┌────────────────────────────────┐
        │   中继 relay/server.py         │  ← 任意一台有公网 IP 的 VPS
        │   控制口/数据口:7000 (单端口)   │     Python 3 标准库，零依赖
        └──────▲──────────▲─────────────┘
   控制通道:7000│          │数据通道:7000（每个访客一条，板子主动出站）
               │  全部出站 TCP —— NAT 后面也能用
        ┌──────┴──────────┴─────────────┐
        │   AI-M61-32S (BL618 板子)      │  ← 连家里 WiFi，通电即用
        │   firmware/ (FreeRTOS+lwIP)    │
        └───────────────┬───────────────┘
                        │ 局域网 TCP
                        ▼
              内网服务（HTTP/SSH/任意 TCP）
```

## 特性

- **真·MCU 级 ngrok**：固件 ~800KB、运行内存 ~90KB 空闲，跑在 FreeRTOS + lwIP 上
- **多目标透传**：一张映射表（公网端口 → 内网 IP:端口），支持任意 TCP 服务（HTTP/SSH/数据库调试口）
- **板载 Web 管理页**：增删端口映射、换 WiFi、换服务器地址、改管理密码/token——全部网页操作，保存即生效，**不用重烧固件**
- **AP 配网**：连不上 WiFi 时板子自动开热点 `M61-Setup`，手机连上去浏览器打开 `192.168.1.1` 就能配网（像配智能插座一样）
- **状态灯**：红/绿/蓝三色 LED 一眼判断板子死在哪一段
- **断电记忆**：所有配置（WiFi、服务器、映射表、密码）存 flash，重启即恢复
- **SOCKS5 模式（类 VPN）**：客户端配 SOCKS5 代理指向中继，直接访问板子所在内网的任意 IP:端口——无需逐个加映射，复用 7000 单端口，详见 [docs/socks5.md](docs/socks5.md)
- **单端口协议**：板子出站只占 1 个端口（默认 7000），适合只放行少数端口的严格网络
- **中继零依赖**：纯 Python 3 标准库（≥3.8），VPS 上一个文件跑起来，systemd 托管

## 快速开始

### 第 0 步：不用板子，先在电脑上体验（5 分钟）

`tools/fake_board.py` 是 PC 版客户端（协议与固件完全一致），可以先用它验证你的 VPS 和网络环境：

```bash
# 终端1：VPS 上（或本机测试）
python3 relay/server.py --token $(openssl rand -hex 16) --single-port 7000

# 终端2：电脑上，把本机 8000 端口映射到公网
python3 tools/fake_board.py --relay-host <vps-ip> --token <同一个token> --target 127.0.0.1:8000

# 手机开流量访问 http://<vps-ip>:7000/  ← 就是本机 8000 的服务
```

多目标模式：`--tunnels "21114=127.0.0.1:8080,21115=192.168.1.100:3000"`

### 第 1 步：部署中继（VPS）

```bash
# 任意 Linux + Python ≥3.8
TOKEN=$(openssl rand -hex 16)   # 记下来！
python3 relay/server.py --token "$TOKEN" --single-port 7000

# 开机自启（改好 token 后）
sudo cp deploy/m61grok.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now m61grok
```

防火墙：7000 对全网开放（板子+访客都走它）；v2 多目标模式的访客端口（如 21114-21119）按需开放。

### 第 2 步：编译固件

基于 [bouffalo_sdk](https://gitee.com/bouffalolab/bouffalo_sdk)（gitee 镜像国内快）：

```bash
# 依赖：cmake ≥3.15、make、python3（Ubuntu: sudo apt install build-essential cmake ninja-build）
git clone --depth 1 https://gitee.com/bouffalolab/toolchain_gcc_t-head_linux.git ~/toolchain
export PATH=~/toolchain/bin:$PATH
git clone --depth 1 https://gitee.com/bouffalolab/bouffalo_sdk.git
cp -r firmware bouffalo_sdk/examples/wifi/sta/m61grok

# 改配置（WiFi/服务器/token，也可以先不改，烧好后网页改）
vi bouffalo_sdk/examples/wifi/sta/m61grok/tunnel_config.h

cd bouffalo_sdk/examples/wifi/sta/m61grok
make CHIP=bl616 BOARD=bl616dk     # AI-M61 系列（BL616/BL618）统一用这组
```

### 第 3 步：烧录

AI-M61-32S-Kit 板载 USB 串口，插 Type-C 即可：

```bash
sudo usermod -aG dialout $USER    # 串口权限，重新登录生效
make flash CHIP=bl616 BOARD=bl616dk COMX=/dev/ttyUSB0
# 卡在握手时按一下板上 RST；CH340 串口报波特率错误就加 BAUDRATE=460800
# 若报 "image file is not existed"（烧录工具给固件名加了 _0 后缀）：
#   cp build/build_out/m61grok_bl616.bin build/build_out/m61grok_bl616_0.bin 后重试
# 烧录完成工具不复位板子，按一次 RST 启动新固件
```

看日志：`python3 -m serial.tools.miniterm /dev/ttyUSB0 2000000`（无输出试 115200；退出 Ctrl+]）

### 第 4 步：配网

三种方式任选：

1. **编译时写死**（tunnel_config.h）——上电直接连；
2. **AP 配网**（推荐）：板子连不上 WiFi 60 秒后自动开热点 `M61-Setup`（密码 `12345678`），手机连上后浏览器打开 `http://192.168.1.1/?pw=m61pin`，在「WiFi」区填你家 WiFi 提交；
3. **串口**：`wifi_sta_connect <ssid> <密码>`（连上后自动记住）。

### 第 5 步：日常使用

| 公网入口 | 内容 |
|---|---|
| `http://<vps-ip>:21114/` | 映射表第 1 条的内网服务 |
| `http://<vps-ip>:21115/` | 映射表第 2 条的内网服务 |
| `http://<vps-ip>:21116/?pw=m61pin` | 板载管理页（改映射/WiFi/服务器/密码都在这） |

局域网内也可以直接访问 `http://<板子IP>/?pw=m61pin`（板子 IP 见串口日志或路由器）。串口敲 `info` 随时查看服务器地址、token、管理密码。

## 状态灯

| 灯 | 含义 |
|---|---|
| 🔴 红常亮 | 未连上 WiFi（信号/密码/路由器问题） |
| 🟢 绿闪烁 | 正在连接 WiFi |
| 🔴 红闪烁 | 配网热点开着，等你手机连 `M61-Setup` |
| 🔵 蓝闪烁 | WiFi 正常，但连不上服务器（查 VPS / 网络） |
| 全灭 | 一切正常，公网可访问 |

## 协议（板 ↔ 中继，行文本 + 裸 TCP）

```
板→中继: HELLO <token> <id> TUNNELS <vport=host:port,...>\n   → OK <port>
中继→板: OPEN <tid> <cid>\n          ← 有访客，来连数据口
板→中继: AUTH <token> <tid> <cid>\n  → OK，之后裸字节双向透传
心跳:    PING/PONG 每 15s
```

单端口模式（默认）：控制、数据、访客全部走 7000，按首行分流（`HELLO`/`AUTH`/其他=HTTP 状态页）。`tools/fake_board.py` 是协议参考实现。

## 安全须知

- token 是公网隧道的钥匙，用 `openssl rand -hex 16` 生成，**不要用默认值**
- 管理页密码（默认 `m61pin`）上板后第一件事改掉（管理页「安全」区）
- 改 token 需要同步修改服务器中继并重启（页面有红字警告）
- 只映射确有必要的服务；映射数据库/SSH 时建议再加一层应用层认证
- 板↔中继 v1 为明文 TCP，敏感流量请自行加 TLS（`--tls-cert/--tls-key`）或走 WireGuard

## 已知限制

- 每目标并发连接默认 4（`CFG_MAX_TUNNELS`，受内存限制）
- 板↔中继明文（token 明文传输）
- demo/管理页为 HTTP/1.0 短连接
- 板子仅支持 2.4G WiFi

## 硬件与软件要求

- **板子**：AI-M61-32S / AI-M61-32S-Kit（BL618，RISC-V 320MHz，320KB RAM / 8MB Flash）。同芯片的其他 BL616/BL618 板改一下 `BOARD=` 大概率也能跑
- **VPS**：任意有公网 IP 的 Linux，Python ≥ 3.8
- **编译**：Linux/WSL，cmake ≥ 3.15，RISC-V 工具链（SDK 同源 gitee 下载）

## FAQ

**为什么必须要一台 VPS？** 板子在 NAT 后面，公网无法主动连它；ngrok/frp/Tailscale 同理都需要一台公网中继（或双方都装客户端的 overlay 网络）。本项目的意义：中继和协议完全自主，没有第三方配额和审查。

**和 frp 什么区别？** frpc 是 Go 程序跑不进 MCU；本协议为 MCU 设计（行文本+裸 TCP，RAM 占用 KB 级）。PC 上日常使用两者都行，本项目的 `fake_board.py` 也能当 PC 版客户端。

**Arduino 能开发吗？** 目前不行——官方 Arduino 核心（bouffalolab/arduino-bouffalo）尚无 WiFiClient/WiFiServer 实现，固件基于 C SDK。

**访客端口怎么选？** 板子映射表里的 `vport` 就是 VPS 上对公网监听的端口，记得在云安全组放行；板子出站永远只用 7000（或你设置的单端口），不受映射表影响。

## License

MIT — 见 [LICENSE](LICENSE)
