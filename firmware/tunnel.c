/*
 * tunnel.c —— m61-tunnel v2 板端核心
 *
 * 能力：
 *   1. 多目标透传：一张映射表（公网访客端口 → 局域网目标），保存在 flash DATA 分区
 *   2. 板载 HTTP 管理页：增删映射、保存并热应用（重连中继下发新表），不用重烧固件
 *   3. 协议 v2：HELLO 携带 TUNNELS 表；OPEN/AUTH 消息带目标编号 tid
 *
 * 协议（与 relay/server.py、tools/fake_board.py 对应）：
 *   板→中继:  HELLO <token> <id> TUNNELS <vport=host:port,...>\n   → OK <port>
 *   中继→板:  OPEN <tid> <cid>\n
 *   板→中继:  AUTH <token> <tid> <cid>\n  → OK\n  之后裸字节透传
 *   PING/PONG 15s 心跳
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <strings.h>
#include <stdarg.h>

#define TUNNEL_FW_VERSION "v3.2"

/* 配网热点 */
#define CFG_AP_SSID "M61-Setup"
#define CFG_AP_PASS "12345678"

/* 板载 LED（Zephyr 板级定义，高电平亮）：红=12 绿=14 蓝=15 */
#include "bflb_gpio.h"
#define LED_R GPIO_PIN_12
#define LED_G GPIO_PIN_14
#define LED_B GPIO_PIN_15

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <lwip/opt.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>

#include "bflb_flash.h"

#include "mem.h" /* kfree_size */
#include "wifi_mgmr_ext.h" /* wifi_sta_connect/disconnect（管理页换 WiFi 用） */

#define DBG_TAG "TUNNEL"
#include "log.h"

#include "tunnel.h"
#include "tunnel_config.h"

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */

static uint32_t now_ms(void)
{
    return (uint32_t)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void sock_set_rcvtimeo(int fd, uint32_t ms)
{
#if LWIP_SO_SNDRCVTIMEO_NONSTANDARD
    int t = (int)ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (void *)&t, sizeof(t));
#else
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (void *)&tv, sizeof(tv));
#endif
}

static void sock_set_sndtimeo(int fd, uint32_t ms)
{
#if LWIP_SO_SNDRCVTIMEO_NONSTANDARD
    int t = (int)ms;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (void *)&t, sizeof(t));
#else
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (void *)&tv, sizeof(tv));
#endif
}

static int tcp_connect_host(const char *host, uint16_t port)
{
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);

    uint32_t ip = inet_addr(host);
    if (ip == 0 || ip == 0xFFFFFFFFUL) {
        struct hostent *he = gethostbyname(host);
        if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
            return -1;
        }
        memcpy(&dst.sin_addr, he->h_addr_list[0], 4);
    } else {
        dst.sin_addr.s_addr = ip;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        closesocket(fd);
        return -1;
    }
    return fd;
}

static int tcp_listen(uint16_t port)
{
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = INADDR_ANY;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        listen(fd, 4) != 0) {
        closesocket(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const uint8_t *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n = send(fd, buf + off, len - off, 0);
        if (n <= 0) {
            return -1;
        }
        off += n;
    }
    return 0;
}

static int send_str(int fd, const char *s)
{
    return send_all(fd, (const uint8_t *)s, strlen(s));
}

/* 行读取（逐字节，不越过行尾）。返回行长度，EOF/超时 -1 */
static int recv_line(int fd, char *line, int maxlen, uint32_t timeout_ms)
{
    int len = 0;
    sock_set_rcvtimeo(fd, timeout_ms);
    while (len < maxlen - 1) {
        char c;
        int n = recv(fd, (uint8_t *)&c, 1, 0);
        if (n != 1) {
            return -1;
        }
        if (c == '\n') {
            if (len > 0 && line[len - 1] == '\r') {
                len--;
            }
            line[len] = '\0';
            return len;
        }
        line[len++] = c;
    }
    line[len] = '\0';
    return len;
}

static int recv_n(int fd, uint8_t *buf, int want, uint32_t timeout_ms)
{
    sock_set_rcvtimeo(fd, timeout_ms);
    int got = 0;
    while (got < want) {
        int n = recv(fd, buf + got, want - got, 0);
        if (n <= 0) {
            return got > 0 ? got : -1;
        }
        got += n;
    }
    return got;
}

/* ------------------------------------------------------------------ */
/* 映射表（RAM + flash 持久化）                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t visitor_port;        /* VPS 上对公网监听的端口 */
    char target_host[48];         /* 目标 IP；"local" = 板载管理页 */
    uint16_t target_port;
    uint8_t in_use;
} tunnel_map_t;

static tunnel_map_t s_map[CFG_MAX_TARGETS];

/* WiFi 凭据也存 flash（管理页可改，换 WiFi 不用重烧固件） */
static char s_wifi_ssid[33];
static char s_wifi_pass[65];

/* 中继服务器地址也存 flash（管理页可改，换服务器不用重烧固件） */
static char s_relay_host[48];
static uint16_t s_relay_port;

/* token 与管理页 PIN 也可管理页修改（存 flash；改 token 需同步服务器侧） */
static char s_token[65];
static char s_pin[24];

/* SOCKS5 开关（管理页控制，经 HELLO 下发给中继；默认关，手动开启） */
static uint8_t s_socks_on = 0;

/* 管理页 HTTP 处理串行锁：static 缓冲不允许并发（多访客同时打开会互相踩） */
static SemaphoreHandle_t s_mgmt_lock;

/* 前向声明（map_load 首次会写默认表进 flash；mgmt 保存后触发重连） */
int map_save(void);
void tunnel_apply_now(void);
void tunnel_wifi_connect(void);
void tunnel_print_info(void);

typedef struct {
    char magic[4];                /* "M61Y"（v3.2：socks默认关；旧布局读不过会回默认一次） */
    uint16_t count;
    char wifi_ssid[33];
    char wifi_pass[65];
    char relay_host[48];
    uint16_t relay_port;
    char token[65];
    char pin[24];
    uint8_t socks_on;
    tunnel_map_t entries[CFG_MAX_TARGETS];
    uint32_t crc;                 /* 简单字节和校验 */
} cfg_blob_t;

static uint32_t blob_sum(const cfg_blob_t *b)
{
    const uint8_t *p = (const uint8_t *)b;
    uint32_t s = 0;
    uint32_t n = offsetof(cfg_blob_t, crc);
    for (uint32_t i = 0; i < n; i++) {
        s = s * 31 + p[i];
    }
    return s;
}

static void map_defaults(void)
{
    memset(s_map, 0, sizeof(s_map));
    strncpy(s_wifi_ssid, CFG_WIFI_SSID, sizeof(s_wifi_ssid) - 1);
    strncpy(s_wifi_pass, CFG_WIFI_PASS, sizeof(s_wifi_pass) - 1);
    strncpy(s_relay_host, CFG_RELAY_HOST, sizeof(s_relay_host) - 1);
    s_relay_port = CFG_RELAY_CTRL_PORT;
    strncpy(s_token, CFG_TOKEN, sizeof(s_token) - 1);
    strncpy(s_pin, CFG_MGMT_PIN, sizeof(s_pin) - 1);
    s_socks_on = 0;
    const tunnel_map_t def[] = { CFG_DEFAULT_MAP };
    int n = sizeof(def) / sizeof(def[0]);
    if (n > CFG_MAX_TARGETS) {
        n = CFG_MAX_TARGETS;
    }
    for (int i = 0; i < n; i++) {
        s_map[i] = def[i];
        s_map[i].in_use = 1;
    }
}

static void map_load(void)
{
    static cfg_blob_t blob;
    bflb_flash_read(CFG_CFG_FLASH_ADDR, (uint8_t *)&blob, sizeof(blob));
    if (memcmp(blob.magic, "M61Y", 4) == 0 &&
        blob.count > 0 && blob.count <= CFG_MAX_TARGETS &&
        blob.crc == blob_sum(&blob)) {
        memset(s_map, 0, sizeof(s_map));
        blob.wifi_ssid[sizeof(blob.wifi_ssid) - 1] = '\0';
        blob.wifi_pass[sizeof(blob.wifi_pass) - 1] = '\0';
        blob.relay_host[sizeof(blob.relay_host) - 1] = '\0';
        strncpy(s_wifi_ssid, blob.wifi_ssid, sizeof(s_wifi_ssid) - 1);
        strncpy(s_wifi_pass, blob.wifi_pass, sizeof(s_wifi_pass) - 1);
        if (blob.relay_host[0] != '\0' && blob.relay_port > 0) {
            strncpy(s_relay_host, blob.relay_host, sizeof(s_relay_host) - 1);
            s_relay_port = blob.relay_port;
        }
        if (blob.token[0] != '\0') {
            strncpy(s_token, blob.token, sizeof(s_token) - 1);
        }
        if (blob.pin[0] != '\0') {
            strncpy(s_pin, blob.pin, sizeof(s_pin) - 1);
        }
        s_socks_on = blob.socks_on ? 1 : 0;
        for (int i = 0; i < blob.count && i < CFG_MAX_TARGETS; i++) {
            blob.entries[i].target_host[sizeof(blob.entries[i].target_host) - 1] = '\0';
            s_map[i] = blob.entries[i];
        }
        LOG_I("config loaded from flash: %d targets, wifi=%s", (int)blob.count, s_wifi_ssid);
        return;
    }
    LOG_I("config: flash invalid, use defaults");
    map_defaults();
    /* 顺手写一份进 flash，管理页才能在此基础上改 */
    map_save();
}

int map_save(void)
{
    static cfg_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    memcpy(blob.magic, "M61Y", 4);
    strncpy(blob.wifi_ssid, s_wifi_ssid, sizeof(blob.wifi_ssid) - 1);
    strncpy(blob.wifi_pass, s_wifi_pass, sizeof(blob.wifi_pass) - 1);
    strncpy(blob.relay_host, s_relay_host, sizeof(blob.relay_host) - 1);
    blob.relay_port = s_relay_port;
    strncpy(blob.token, s_token, sizeof(blob.token) - 1);
    strncpy(blob.pin, s_pin, sizeof(s_pin) - 1);
    for (int i = 0; i < CFG_MAX_TARGETS; i++) {
        if (s_map[i].in_use && blob.count < CFG_MAX_TARGETS) {
            blob.entries[blob.count++] = s_map[i];
        }
    }
    blob.crc = blob_sum(&blob);
    bflb_flash_erase(CFG_CFG_FLASH_ADDR, 4096);
    int r = bflb_flash_write(CFG_CFG_FLASH_ADDR, (uint8_t *)&blob, sizeof(blob));
    LOG_I("config saved to flash (%d targets, wifi=%s) ret=%d",
          (int)blob.count, s_wifi_ssid, r);
    return r;
}

/* 供 main.c 自动连使用：用 flash 里保存的 WiFi */
void tunnel_wifi_connect(void)
{
    tunnel_init();
    tunnel_led_wifi_connecting(1); /* 绿灯闪：开始连 WiFi */
    LOG_I("connecting WiFi \"%s\" ...", s_wifi_ssid);
    int ret = wifi_sta_connect(s_wifi_ssid, s_wifi_pass, NULL, NULL, 0, 0, 0, 1);
    LOG_I("wifi_sta_connect ret=%d", ret);
}

/* 网页"换WiFi"的实际执行体：独立任务跑，避开 HTTP/事件上下文
 * （wifi_sta_disconnect/connect 不能在回调上下文直接调，会触发 FreeRTOS 断言） */
static void wifi_switch_task(void *arg)
{
    (void)arg;
    vTaskDelay(800 / portTICK_PERIOD_MS); /* 先让响应发到浏览器 */
    wifi_sta_disconnect();
    vTaskDelay(500 / portTICK_PERIOD_MS);
    tunnel_wifi_connect();
    vTaskDelete(NULL);
}

/* GOT_IP 时调用：把当前实际连接的 WiFi 固化到 flash。
 * 这样不管 WiFi 是自动连的还是串口 wifi_sta_connect 手动连的，下次开机都直接用。 */
void tunnel_wifi_save_current(void)
{
    wifi_mgmr_connect_ind_stat_info_t st;
    if (wifi_mgmr_sta_connect_ind_stat_get(&st) != 0 || st.ssid[0] == '\0') {
        return;
    }
    st.ssid[32] = '\0';
    st.passphr[64] = '\0';
    if (strcmp(st.ssid, s_wifi_ssid) == 0 && strcmp(st.passphr, s_wifi_pass) == 0) {
        return; /* 和已保存的一致 */
    }
    memset(s_wifi_ssid, 0, sizeof(s_wifi_ssid));
    strncpy(s_wifi_ssid, st.ssid, sizeof(s_wifi_ssid) - 1);
    memset(s_wifi_pass, 0, sizeof(s_wifi_pass));
    strncpy(s_wifi_pass, st.passphr, sizeof(s_wifi_pass) - 1);
    map_save();
    LOG_I("wifi credentials auto-saved: %s", s_wifi_ssid);
}

/* HELLO 里 TUNNELS 字段的序列化：21114=192.168.0.127:8080,21116=local:80 */
static void map_serialize(char *out, int outlen)
{
    int off = 0;
    out[0] = '\0';
    for (int i = 0; i < CFG_MAX_TARGETS; i++) {
        if (!s_map[i].in_use) {
            continue;
        }
        off += snprintf(out + off, outlen - off, "%s%u=%s:%u",
                        off ? "," : "",
                        (unsigned)s_map[i].visitor_port,
                        s_map[i].target_host,
                        (unsigned)s_map[i].target_port);
        if (off >= outlen - 24) {
            break;
        }
    }
}

/* 表里目标总条数 */
static int map_count(void)
{
    int n = 0;
    for (int i = 0; i < CFG_MAX_TARGETS; i++) {
        n += s_map[i].in_use ? 1 : 0;
    }
    return n;
}

/* 优雅关闭：先 shutdown 通知对端"发完了"，等对方收尾后再 close。
 * 直接 close 会把 TCP 发送队列里未确认的数据 RST 掉，
 * 浏览器报 ERR_CONTENT_LENGTH_MISMATCH / ERR_CONNECTION_RESET。 */
static void sock_close_graceful(int fd)
{
    if (fd < 0) {
        return;
    }
    shutdown(fd, SHUT_WR);
    uint8_t sink[64];
    uint32_t t0 = now_ms();
    sock_set_rcvtimeo(fd, 300);
    while (now_ms() - t0 < 2000) {
        if (recv(fd, sink, sizeof(sink), 0) == 0) {
            break; /* 对端已关闭 */
        }
    }
    closesocket(fd);
}

/* ------------------------------------------------------------------ */
/* HTML 管理页                                                         */
/* ------------------------------------------------------------------ */

/* 简化 urldecode：管理页表单值只含 [0-9a-zA-Z.:-_]，其余原样 */
static void html_escape(const char *in, char *out, int outlen)
{
    int o = 0;
    for (const char *p = in; *p && o < outlen - 6; p++) {
        switch (*p) {
            case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
            case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
            case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
            default: out[o++] = *p;
        }
    }
    out[o] = '\0';
}

static void http_respond(int fd, int code, const char *reason,
                         const char *body, int body_len)
{
    char head[192];
    int hl = snprintf(head, sizeof(head),
                      "HTTP/1.0 %d %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: %d\r\nConnection: close\r\n\r\n",
                      code, reason, body_len);
    int r1 = send_all(fd, (const uint8_t *)head, hl);
    int r2 = r1 == 0 ? send_all(fd, (const uint8_t *)body, body_len) : -1;
    LOG_I("respond %d: head %dB(%d) body %dB(%d)", code, hl, r1, body_len, r2);
}

/* 安全拼接：返回值永远钳制在"实际写入的字节数"，Content-Length 由此而来，
 * 从数学上杜绝 ERR_CONTENT_LENGTH_MISMATCH（snprintf 截断时返回的是期望长度而非实际） */
static int page_append(char *page, int off, int cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(page + off, cap - off, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return off;
    }
    if (n > cap - off - 1) {
        n = cap - off - 1; /* 被截断：只算实际写进去的 */
    }
    return off + n;
}

static void mgmt_page(int fd)
{
    static char page[3072];
    char esc[64];
    int off = 0;
    int cap = (int)sizeof(page);

    off = page_append(page, off, cap,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>m61-tunnel</title>"
        "<style>body{font-family:sans-serif;margin:2em}table,td,th,input{margin:4px;"
        "padding:4px;border:1px solid #ccc;border-collapse:collapse}"
        "button{padding:6px 14px}</style></head><body>"
        "<h2>m61-tunnel 映射管理</h2><table><tr><th>#</th><th>公网端口</th>"
        "<th>内网目标</th><th>操作</th></tr>"
        "<form method='POST' action='/op'>"
        "<input type='hidden' name='pw' value='%s'>", s_pin);

    for (int i = 0; i < CFG_MAX_TARGETS && off < cap - 256; i++) {
        if (!s_map[i].in_use) {
            continue;
        }
        html_escape(s_map[i].target_host, esc, sizeof(esc));
        off = page_append(page, off, cap,
                          "<tr><td>%d</td><td>:%u</td><td>%s:%u</td>"
                          "<td><button name='op' value='del:%d'>删除</button></td></tr>",
                          i, (unsigned)s_map[i].visitor_port, esc,
                          (unsigned)s_map[i].target_port, i);
    }
    const char *socks_op = s_socks_on ? "socks_off" : "socks_on";
    const char *socks_label = s_socks_on ? "SOCKS5: ON (click to disable)"
                                         : "SOCKS5: OFF (click to enable)";
    off = page_append(page, off, cap,
        "</form></table><hr/>"
        "<form method='POST' action='/op'>"
        "<input type='hidden' name='pw' value='%s'>"
        "<input name='vport' size='7' placeholder='公网端口'> "
        "<input name='host' size='16' placeholder='内网IP或local'> "
        "<input name='tport' size='6' placeholder='目标端口'> "
        "<button name='op' value='add'>添加映射</button></form>"
        "<form method='POST' action='/op'>"
        "<input type='hidden' name='pw' value='%s'>"
        "<button name='op' value='save'>保存并应用到中继</button>（保存后立刻生效，无需重启）</form>"
        "<hr/><h3>WiFi</h3>"
        "<form method='POST' action='/op'>"
        "<input type='hidden' name='pw' value='%s'>"
        "<input name='ssid' size='18' placeholder='WiFi名称' value='%s'> "
        "<input name='wifipass' size='18' placeholder='WiFi密码'> "
        "<button name='op' value='wifi'>换到这个WiFi</button></form>"
        "<h3>服务器</h3>"
        "<form method='POST' action='/op'>"
        "<input type='hidden' name='pw' value='%s'>"
        "<input name='rhost' size='18' placeholder='服务器IP' value='%s'> "
        "<input name='rport' size='6' placeholder='端口' value='%u'> "
        "<button name='op' value='relay'>切换到这个服务器</button></form>"
        "<hr/><h3>SOCKS5</h3>"
        "<form method='POST' action='/op'>"
        "<input type='hidden' name='pw' value='%s'>"
        "<button name='op' value='%s'>%s</button></form>"
        "<hr/><h3>安全</h3>"
        "<form method='POST' action='/op'>"
        "<input type='hidden' name='pw' value='%s'>"
        "<input name='newpin' size='10' placeholder='新管理密码'> "
        "<button name='op' value='pin'>改管理密码</button></form>"
        "<form method='POST' action='/op'>"
        "<input type='hidden' name='pw' value='%s'>"
        "<input name='newtoken' size='30' placeholder='新隧道token(改后需同步服务器!)'> "
        "<button name='op' value='token'>改token</button>"
        "<p style='color:#c00'>警告: 改token后必须同步修改服务器并重启中继,"
        "否则隧道断开(蓝灯闪), 需用局域网管理页改回!</p></form>"
        "<p style='color:#888'>fw %s | 中继: %s:%u | 目标数: %d | heap: %d B</p>"
        "</body></html>",
        s_pin, s_pin, s_pin,               /* add/save/wifi 表单的 pw */
        s_wifi_ssid,                        /* wifi ssid 输入框值 */
        s_pin,                              /* relay 表单的 pw */
        s_relay_host, (unsigned)s_relay_port, /* relay 表单 host/port 值 */
        s_pin, socks_op, socks_label,       /* socks 表单 */
        s_pin, s_pin,                       /* pin/token 表单的 pw */
        TUNNEL_FW_VERSION,                  /* 页脚 */
        s_relay_host, (unsigned)s_relay_port, map_count(), kfree_size());

    http_respond(fd, 200, "OK", page, off);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 简化 urldecode：+ 转空格、%HH 转字节（SSID 可能带中文/空格） */
static void urldecode_inplace(char *s)
{
    char *o = s;
    for (const char *p = s; *p; p++) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (p[0] == '%' && p[1] && p[2]) {
            int hi = hexval(p[1]), lo = hexval(p[2]);
            if (hi >= 0 && lo >= 0) {
                *o++ = (char)((hi << 4) | lo);
                p += 2;
            } else {
                *o++ = *p;
            }
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

/* 从 body / url 里找 k=v（自动做 urldecode） */
static int form_value(const char *haystack, const char *key, char *out, int outlen)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(haystack, pat);
    if (!p) {
        return -1;
    }
    p += strlen(pat);
    int o = 0;
    while (*p && *p != '&' && *p != ' ' && *p != '\r' && *p != '\n' && o < outlen - 1) {
        out[o++] = *p++;
    }
    out[o] = '\0';
    urldecode_inplace(out);
    return o;
}

/*
 * 处理一条 HTTP 请求（管理页）。
 * req_head: 请求头（含首行）；body: 请求体（可能为空）
 * 返回 0 正常处理。
 */
static int mgmt_handle(int fd, const char *req_head, const char *body)
{
    static char pw[80];
    char method[8] = "", path[96] = "";

    /* 认证：URL ?pw= 或表单 pw= */
    if (form_value(req_head, "pw", pw, sizeof(pw)) < 0 &&
        form_value(body, "pw", pw, sizeof(pw)) < 0) {
        pw[0] = '\0';
    }
    if (strcmp(pw, s_pin) != 0) {
        static const char msg[] =
            "<html><body><h3>403 密码错误</h3>"
            "<p>URL 后加 ?pw=PIN（忘记请看串口 info 输出）</p></body></html>";
        LOG_W("mgmt: auth failed from visitor");
        http_respond(fd, 403, "Forbidden", msg, sizeof(msg) - 1);
        return 0;
    }

    if (sscanf(req_head, "%7s %95s", method, path) != 2) {
        http_respond(fd, 400, "Bad Request", "bad req", 8);
        return 0;
    }

    if (strcmp(method, "GET") == 0) {
        mgmt_page(fd);
        return 0;
    }

    /* POST /op */
    char op[24] = "";
    form_value(body, "op", op, sizeof(op));
    LOG_I("mgmt: op=%s", op);

    if (strncmp(op, "del:", 4) == 0) {
        int idx = atoi(op + 4);
        if (idx >= 0 && idx < CFG_MAX_TARGETS && s_map[idx].in_use) {
            s_map[idx].in_use = 0;
        }
        mgmt_page(fd);
        return 0;
    }
    if (strcmp(op, "add") == 0) {
        char vport[8], host[48], tport[8];
        if (form_value(body, "vport", vport, sizeof(vport)) > 0 &&
            form_value(body, "host", host, sizeof(host)) > 0 &&
            form_value(body, "tport", tport, sizeof(tport)) > 0) {
            for (int i = 0; i < CFG_MAX_TARGETS; i++) {
                if (!s_map[i].in_use) {
                    s_map[i].visitor_port = (uint16_t)atoi(vport);
                    strncpy(s_map[i].target_host, host, sizeof(s_map[i].target_host) - 1);
                    s_map[i].target_port = (uint16_t)atoi(tport);
                    s_map[i].in_use = 1;
                    break;
                }
            }
        }
        mgmt_page(fd);
        return 0;
    }
    if (strcmp(op, "save") == 0) {
        map_save();
        const char pre[] =
            "<html><body><h3>已保存并开始应用</h3>"
            "<p>固件会带着新映射表重连中继，约 3~10 秒后新端口生效。</p>"
            "<p><a href='/?pw=";
        const char post[] = "'>返回管理页</a></p></body></html>";
        int total = (sizeof(pre) - 1) + strlen(s_pin) + (sizeof(post) - 1);
        char head[160];
        int hl = snprintf(head, sizeof(head),
                          "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n", total);
        if (send_all(fd, (const uint8_t *)head, hl) == 0 &&
            send_str(fd, pre) == 0 &&
            send_str(fd, s_pin) == 0) {
            send_str(fd, post);
        }
        tunnel_apply_now();
        return 0;
    }

    if (strcmp(op, "wifi") == 0) {
        char ssid[33], pass[65];
        if (form_value(body, "ssid", ssid, sizeof(ssid)) > 0 &&
            form_value(body, "wifipass", pass, sizeof(pass)) > 0) {
            memset(s_wifi_ssid, 0, sizeof(s_wifi_ssid));
            strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
            memset(s_wifi_pass, 0, sizeof(s_wifi_pass));
            strncpy(s_wifi_pass, pass, sizeof(s_wifi_pass) - 1);
            /* 注意：这里不写 flash——只有真正连上（GOT_IP）才会固化；
             * 连不上时重启即回旧配置，不会把错误 WiFi 写死 */
            const char msg[] =
                "<html><body><h3>OK 正在切换 WiFi</h3>"
                "<p>连上后自动保存并恢复隧道；若 60 秒后仍未恢复，"
                "板子会自动开 M61-Setup 热点供重新配置。</p></body></html>";
            http_respond(fd, 200, "OK", msg, sizeof(msg) - 1);
            xTaskCreate(wifi_switch_task, "wifisw", 1024, NULL, 10, NULL);
            return 0;
        }
        http_respond(fd, 400, "Bad Request", "need ssid+wifipass", 20);
        return 0;
    }

    if (strcmp(op, "relay") == 0) {
        char rhost[48], rport[8];
        if (form_value(body, "rhost", rhost, sizeof(rhost)) > 0 &&
            form_value(body, "rport", rport, sizeof(rport)) > 0 &&
            atoi(rport) > 0 && atoi(rport) < 65536) {
            memset(s_relay_host, 0, sizeof(s_relay_host));
            strncpy(s_relay_host, rhost, sizeof(s_relay_host) - 1);
            s_relay_port = (uint16_t)atoi(rport);
            map_save();
            const char msg[] =
                "<html><body><h3>OK 服务器已保存，正在重连</h3>"
                "<p>若新地址不可达，蓝灯会持续闪烁；此时用局域网管理页改回来即可。</p></body></html>";
            http_respond(fd, 200, "OK", msg, sizeof(msg) - 1);
            tunnel_apply_now();
            return 0;
        }
        http_respond(fd, 400, "Bad Request", "need rhost+rport", 17);
        return 0;
    }

    if (strcmp(op, "socks_on") == 0 || strcmp(op, "socks_off") == 0) {
        s_socks_on = (strcmp(op, "socks_on") == 0);
        map_save();
        const char msg[] = "<html><body><h3>OK</h3>"
            "<p>SOCKS5 已切换并保存，正在通知中继（几秒内生效）。</p></body></html>";
        http_respond(fd, 200, "OK", msg, sizeof(msg) - 1);
        tunnel_apply_now();
        return 0;
    }
    if (strcmp(op, "pin") == 0) {
        char newpin[24];
        if (form_value(body, "newpin", newpin, sizeof(newpin)) > 3 &&
            strlen(newpin) < sizeof(s_pin)) {
            memset(s_pin, 0, sizeof(s_pin));
            strncpy(s_pin, newpin, sizeof(s_pin) - 1);
            map_save();
            const char msg[] = "<html><body><h3>OK 管理密码已改</h3>"
                "<p>新密码已生效并保存。请牢记；忘记时串口敲 info 可查。</p></body></html>";
            http_respond(fd, 200, "OK", msg, sizeof(msg) - 1);
            return 0;
        }
        http_respond(fd, 400, "Bad Request", "pin too short", 15);
        return 0;
    }
    if (strcmp(op, "token") == 0) {
        char newtoken[65];
        if (form_value(body, "newtoken", newtoken, sizeof(newtoken)) > 7 &&
            strlen(newtoken) < sizeof(s_token)) {
            memset(s_token, 0, sizeof(s_token));
            strncpy(s_token, newtoken, sizeof(s_token) - 1);
            map_save();
            const char msg[] = "<html><body><h3>OK token 已改，正在用新 token 重连</h3>"
                "<p><b>现在必须同步修改服务器中继的 token 并重启它</b>，"
                "否则蓝灯常闪（连不上服务器）。改错可用局域网管理页改回。</p></body></html>";
            http_respond(fd, 200, "OK", msg, sizeof(msg) - 1);
            tunnel_apply_now();
            return 0;
        }
        http_respond(fd, 400, "Bad Request", "token too short (min 8)", 24);
        return 0;
    }

    http_respond(fd, 404, "Not Found", "no such op", 10);
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTTP 请求接收（管理页用，板载监听与隧道 local 目标共用）            */
/* ------------------------------------------------------------------ */

static void http_serve_conn(int fd)
{
    /* static 缓冲 + 互斥锁：管理页可能被多条连接同时打开（公网映射口+局域网口），
     * 串行化处理，否则页面内容互相覆盖导致 Content-Length 对不上。
     * 头部必须读到"空行"为止（浏览器头很长，以前 640B 截断会把剩余头当 body） */
    static char head[1600];
    static char body[512];
    static char line[256];
    if (s_mgmt_lock) {
        xSemaphoreTake(s_mgmt_lock, portMAX_DELAY);
    }
    int hoff = 0, clen = 0, head_done = 0;
    for (int guard = 0; guard < 40; guard++) {
        int n = recv_line(fd, line, sizeof(line), 3000);
        if (n < 0) {
            break; /* 出错/超时 */
        }
        if (n == 0) {
            head_done = 1;
            break; /* 空行 = 头结束 */
        }
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            clen = atoi(line + 15);
        }
        int room = (int)sizeof(head) - 2 - hoff;
        if (room > n) {
            memcpy(head + hoff, line, n);
            hoff += n;
            head[hoff++] = '\n';
        }
        /* 缓冲装不下的超长头行直接丢弃（字段提取已完成即可） */
    }
    if (head_done) {
        head[hoff] = '\0';
        int blen = 0;
        if (clen > 0 && clen < (int)sizeof(body)) {
            blen = recv_n(fd, (uint8_t *)body, clen, 3000);
            if (blen < 0) {
                blen = 0;
            }
            body[blen] = '\0';
        } else {
            body[0] = '\0';
        }
        mgmt_handle(fd, head, body);
    }
    if (s_mgmt_lock) {
        xSemaphoreGive(s_mgmt_lock);
    }
}

/* 板载管理服务（局域网直连）：http://板子IP:CFG_MGMT_PORT/?pw=token */
static void mgmt_server_task(void *arg)
{
    (void)arg;
    int lfd = tcp_listen(CFG_MGMT_PORT);
    if (lfd < 0) {
        LOG_W("mgmt listen %d failed", CFG_MGMT_PORT);
        vTaskDelete(NULL);
        return;
    }
    LOG_I("mgmt server on :%d (LAN)", CFG_MGMT_PORT);
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }
        sock_set_sndtimeo(cfd, 5000);
        http_serve_conn(cfd);
        sock_close_graceful(cfd);
    }
}

/* ------------------------------------------------------------------ */
/* 配网热点（连不上 WiFi 时的兜底入口）                                */
/* ------------------------------------------------------------------ */

static volatile int s_ap_mode = 0;

static int s_tunnel_started = 0; /* GOT_IP 后置位（ap watchdog 据此判断是否需要开热点） */

/* ------------------------------------------------------------------ */
/* 状态灯：红常亮=未连WiFi  绿闪=正在连WiFi  红闪=配网热点              */
/*         蓝闪=WiFi已连但连不上服务端  全灭=一切正常                   */
/* ------------------------------------------------------------------ */

static struct bflb_device_s *s_led_gpio;
static volatile uint8_t s_led_wifi_ok = 0;
static volatile uint8_t s_led_wifi_connecting = 0;
static volatile uint8_t s_led_tunnel_up = 0;

void tunnel_led_wifi(int ok)
{
    s_led_wifi_ok = ok ? 1 : 0;
    if (ok) {
        s_led_wifi_connecting = 0; /* 拿到 IP，连接过程结束 */
    }
}

void tunnel_led_wifi_connecting(int on)
{
    s_led_wifi_connecting = on ? 1 : 0;
}

void tunnel_led_tunnel(int up)
{
    s_led_tunnel_up = up ? 1 : 0;
}

static void led_task(void *arg)
{
    (void)arg;
    s_led_gpio = bflb_device_get_by_name("gpio");
    bflb_gpio_init(s_led_gpio, LED_R, GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0);
    bflb_gpio_init(s_led_gpio, LED_G, GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0);
    bflb_gpio_init(s_led_gpio, LED_B, GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0);
    int phase = 0;
    for (;;) {
        int r_on = 0, g_on = 0, b_on = 0;
        if (s_ap_mode) {                       /* 配网热点：红闪 */
            r_on = phase;
        } else if (s_led_wifi_connecting && !s_led_wifi_ok) {
            g_on = phase;                      /* 正在连 WiFi：绿闪 */
        } else if (!s_led_wifi_ok) {
            r_on = 1;                          /* 未连 WiFi：红常亮 */
        } else if (!s_led_tunnel_up) {
            b_on = phase;                      /* WiFi OK 但服务端未连：蓝闪 */
        }                                      /* 全部正常：全灭 */
        if (r_on) bflb_gpio_set(s_led_gpio, LED_R); else bflb_gpio_reset(s_led_gpio, LED_R);
        if (g_on) bflb_gpio_set(s_led_gpio, LED_G); else bflb_gpio_reset(s_led_gpio, LED_G);
        if (b_on) bflb_gpio_set(s_led_gpio, LED_B); else bflb_gpio_reset(s_led_gpio, LED_B);
        phase ^= 1;
        vTaskDelay(250 / portTICK_PERIOD_MS);
    }
}

void tunnel_ap_start(void)
{
    static wifi_mgmr_ap_params_t ap; /* static：lib 可能持有字段指针 */
    if (s_ap_mode) {
        return;
    }
    memset(&ap, 0, sizeof(ap));
    ap.ssid = CFG_AP_SSID;
    ap.key = CFG_AP_PASS;
    ap.akm = "WPA2";
    ap.channel = 6;
    ap.use_dhcpd = true; /* lib 内部自动起 DHCP，给手机分 IP */
    LOG_W("starting setup AP \"%s\" (pass %s), open mgmt page to config wifi",
          CFG_AP_SSID, CFG_AP_PASS);
    if (wifi_mgmr_ap_start(&ap) == 0) {
        s_ap_mode = 1;
    }
}

void tunnel_ap_stop(void)
{
    if (s_ap_mode) {
        s_ap_mode = 0;
        wifi_mgmr_ap_stop();
        LOG_I("setup AP stopped");
    }
}

int tunnel_ap_active(void)
{
    return s_ap_mode;
}

/* 常驻看门狗：持续 60 秒 WiFi 不在线（任何时刻开始算）→ 自动开配网热点。
 * 以前只在上电后查一次，中途改错 WiFi 掉线就永远没热点了。 */
static void ap_watchdog_task(void *arg)
{
    (void)arg;
    uint32_t last_online = now_ms();
    for (;;) {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        if (s_led_wifi_ok) {
            last_online = now_ms();
            if (s_ap_mode) {
                tunnel_ap_stop(); /* 联网成功关热点：必须在独立任务上下文做，
                                     事件回调里直接调会触发 FreeRTOS 断言崩溃 */
            }
        }
        if (!s_ap_mode && !s_led_wifi_ok &&
            now_ms() - last_online > 60000) {
            tunnel_ap_start();
            last_online = now_ms(); /* 开完重置计时，配网失败也不会反复开关 */
        }
    }
}

#ifdef CONFIG_SHELL
#include "shell.h"
static void cmd_apmode(int argc, char **argv)
{
    tunnel_ap_start();
}
SHELL_CMD_EXPORT_ALIAS(cmd_apmode, apmode, start m61 setup AP);

/* 串口敲 info 随时重看连接信息 */
static void cmd_info(int argc, char **argv)
{
    tunnel_print_info();
}
SHELL_CMD_EXPORT_ALIAS(cmd_info, info, print m61-tunnel info);
#endif

/* ------------------------------------------------------------------ */
/* 隧道槽位                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile int in_use;
    uint32_t cid;
    int map_idx;    /* 目标在 s_map 的下标；-1 = SOCKS5 动态目标 */
    char dyn_host[48]; /* SOCKS5 动态目标（中继下发的任意内网地址） */
    uint16_t dyn_port;
    int relay_fd;
    int local_fd;
    uint32_t last_ms;
    uint8_t buf[CFG_BUF_SIZE];
} tunnel_slot_t;

static tunnel_slot_t s_slots[CFG_MAX_TUNNELS];

static void tunnel_data_task(void *arg)
{
    tunnel_slot_t *s = (tunnel_slot_t *)arg;
    char line[128];
    const tunnel_map_t *m = (s->map_idx >= 0 && s->map_idx < CFG_MAX_TARGETS) ?
                             &s_map[s->map_idx] : NULL;

    do {
        s->relay_fd = tcp_connect_host(s_relay_host, s_relay_port);
        if (s->relay_fd < 0) {
            break;
        }
        if (s->map_idx >= 0) {
            snprintf(line, sizeof(line), "AUTH %s %d %lu\n", s_token,
                     s->map_idx, (unsigned long)s->cid);
        } else {
            snprintf(line, sizeof(line), "AUTHX %s %lu %s %u\n", s_token,
                     (unsigned long)s->cid, s->dyn_host, (unsigned)s->dyn_port);
        }
        if (send_str(s->relay_fd, line) != 0) {
            break;
        }
        if (recv_line(s->relay_fd, line, sizeof(line), 5000) < 0 ||
            strncmp(line, "OK", 2) != 0) {
            LOG_W("cid %lu AUTH failed: %s", (unsigned long)s->cid, line);
            break;
        }

        int is_local = (m && strcmp(m->target_host, "local") == 0);
        if (s->map_idx < 0) { /* SOCKS5 动态目标 */
            s->local_fd = tcp_connect_host(s->dyn_host, s->dyn_port);
            if (s->local_fd < 0) {
                LOG_W("cid %lu socks target %s:%u unreachable",
                      (unsigned long)s->cid, s->dyn_host, (unsigned)s->dyn_port);
                break;
            }
            LOG_I("cid %lu socks up: -> %s:%u", (unsigned long)s->cid,
                  s->dyn_host, (unsigned)s->dyn_port);
        } else if (!is_local && m) {
            s->local_fd = tcp_connect_host(m->target_host, m->target_port);
            if (s->local_fd < 0) {
                LOG_W("cid %lu target %s:%u unreachable",
                      (unsigned long)s->cid, m->target_host, (unsigned)m->target_port);
                break;
            }
            sock_set_rcvtimeo(s->local_fd, 100);
            sock_set_sndtimeo(s->local_fd, 5000);
        }
        sock_set_rcvtimeo(s->relay_fd, 100);
        sock_set_sndtimeo(s->relay_fd, 5000);
        {
            int on = 1;
            setsockopt(s->relay_fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        }

        LOG_I("cid %lu tunnel up: :%u -> %s:%u", (unsigned long)s->cid,
              m ? (unsigned)m->visitor_port : 0u,
              m ? m->target_host : "?", m ? (unsigned)m->target_port : 0u);

        if (is_local) {
            /* 目标=板载管理页：直接在本连接上跑 HTTP */
            http_serve_conn(s->relay_fd);
            /* 给 TCP 一点把剩余数据发完的时间再关（防御 close 丢尾巴） */
            vTaskDelay(300 / portTICK_PERIOD_MS);
            break;
        }

        for (;;) {
            int n = recv(s->relay_fd, s->buf, sizeof(s->buf), 0);
            if (n == 0) {
                break;
            }
            if (n > 0) {
                s->last_ms = now_ms();
                if (s->local_fd < 0 || send_all(s->local_fd, s->buf, n) != 0) {
                    break;
                }
            }
            if (s->local_fd >= 0) {
                int m2 = recv(s->local_fd, s->buf, sizeof(s->buf), 0);
                if (m2 == 0) {
                    break;
                }
                if (m2 > 0) {
                    s->last_ms = now_ms();
                    if (send_all(s->relay_fd, s->buf, m2) != 0) {
                        break;
                    }
                }
            }
            if (now_ms() - s->last_ms > CFG_IDLE_TIMEOUT_MS) {
                break;
            }
        }
    } while (0);

    LOG_I("cid %lu tunnel closed", (unsigned long)s->cid);
    {
        int rfd = s->relay_fd, lfd2 = s->local_fd;
        s->relay_fd = -1;
        s->local_fd = -1;
        sock_close_graceful(rfd);
        if (lfd2 >= 0) {
            closesocket(lfd2);
        }
    }
    s->in_use = 0;
    vTaskDelete(NULL);
}

static void handle_open(int map_idx, uint32_t cid, const char *dyn_host, uint16_t dyn_port)
{
    if (map_idx >= 0 && (map_idx >= CFG_MAX_TARGETS || !s_map[map_idx].in_use)) {
        LOG_W("OPEN bad map_idx %d", map_idx);
        return;
    }
    if (map_idx < 0 && (dyn_host == NULL || dyn_port == 0)) {
        LOG_W("OPENX missing target");
        return;
    }
    for (int i = 0; i < CFG_MAX_TUNNELS; i++) {
        if (!s_slots[i].in_use) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_slots[i].in_use = 1;
            s_slots[i].cid = cid;
            s_slots[i].map_idx = map_idx;
            if (map_idx < 0) {
                strncpy(s_slots[i].dyn_host, dyn_host, sizeof(s_slots[i].dyn_host) - 1);
                s_slots[i].dyn_port = dyn_port;
            }
            s_slots[i].relay_fd = -1;
            s_slots[i].local_fd = -1;
            s_slots[i].last_ms = now_ms();
            if (xTaskCreate(tunnel_data_task, "tun", 1024,
                            (void *)&s_slots[i], 12, NULL) != pdPASS) {
                LOG_W("create tunnel task failed");
                s_slots[i].in_use = 0;
            }
            return;
        }
    }
    LOG_W("cid %lu dropped: no free slot", (unsigned long)cid);
}

/* ------------------------------------------------------------------ */
/* 控制通道                                                            */
/* ------------------------------------------------------------------ */

static int s_ctrl_fd = -1;
static volatile int s_apply_request = 0;

void tunnel_apply_now(void)
{
    s_apply_request = 1;
    if (s_ctrl_fd >= 0) {
        closesocket(s_ctrl_fd); /* 触发控制任务重连，重连时带新表 */
        s_ctrl_fd = -1;
    }
}

static void tunnel_task(void *arg)
{
    char line[320];
    char tunnels_field[224];
    uint32_t last_ping = 0, last_pong = 0, backoff_ms = 2000;

    (void)arg;
    tunnel_init(); /* 幂等；正常路径在 main() 里已加载 */
    LOG_I("m61-tunnel " TUNNEL_FW_VERSION " build " __DATE__ " " __TIME__);

    for (;;) {
        map_serialize(tunnels_field, sizeof(tunnels_field));
        s_ctrl_fd = tcp_connect_host(s_relay_host, s_relay_port);
        if (s_ctrl_fd < 0) {
            vTaskDelay(backoff_ms / portTICK_PERIOD_MS);
            backoff_ms = backoff_ms < 60000 ? backoff_ms * 2 : 60000;
            continue;
        }
        backoff_ms = 2000;
        s_apply_request = 0;

        snprintf(line, sizeof(line), "HELLO %s %s TUNNELS %s SOCKS=%d\n",
                 s_token, CFG_TUNNEL_ID, tunnels_field, s_socks_on);
        if (send_str(s_ctrl_fd, line) != 0 ||
            recv_line(s_ctrl_fd, line, sizeof(line), 5000) < 0 ||
            strncmp(line, "OK", 2) != 0) {
            LOG_W("HELLO failed: %s", line);
            closesocket(s_ctrl_fd);
            s_ctrl_fd = -1;
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            continue;
        }
        LOG_I("control up (%s) with %d tunnels", line, map_count());
        tunnel_led_tunnel(1); /* 绿灯闪：隧道已建立 */
        last_ping = last_pong = now_ms();
        sock_set_rcvtimeo(s_ctrl_fd, 500);

        for (;;) {
            uint32_t now = now_ms();
            if (now - last_ping >= 15000) {
                if (send_str(s_ctrl_fd, "PING\n") != 0) {
                    break;
                }
                last_ping = now;
            }
            if (now - last_pong > 45000) {
                LOG_W("relay not answering PING, reconnect");
                break;
            }
            if (recv_line(s_ctrl_fd, line, sizeof(line), 500) > 0) {
                if (strcmp(line, "PONG") == 0) {
                    last_pong = now_ms();
                } else if (strncmp(line, "OPENX ", 6) == 0) {
                    /* v3 SOCKS5: OPENX <cid> <host> <port> */
                    unsigned long cid = 0;
                    char host[48];
                    unsigned port = 0;
                    if (sscanf(line + 6, "%lu %47s %u", &cid, host, &port) == 3) {
                        handle_open(-1, (uint32_t)cid, host, (uint16_t)port);
                    }
                } else if (strncmp(line, "OPEN ", 5) == 0) {
                    int map_idx = -1;
                    unsigned long cid = 0;
                    if (sscanf(line + 5, "%d %lu", &map_idx, &cid) == 2) {
                        handle_open(map_idx, (uint32_t)cid, NULL, 0);
                    } else if (sscanf(line + 5, "%lu", &cid) == 1) {
                        handle_open(0, (uint32_t)cid, NULL, 0); /* v1 兼容 */
                    }
                }
            }
            if (s_apply_request) {
                LOG_I("config changed, reconnecting to push new map");
                break;
            }
        }

        if (s_ctrl_fd >= 0) {
            closesocket(s_ctrl_fd);
            s_ctrl_fd = -1;
        }
        tunnel_led_tunnel(0); /* 隧道断开：绿灯灭 */
        vTaskDelay(s_apply_request ? 100 : backoff_ms / portTICK_PERIOD_MS);
    }
}

/* ------------------------------------------------------------------ */
/* 对外入口                                                            */
/* ------------------------------------------------------------------ */

/* 尽早调用（main 里 board_init 之后）：从 flash 加载映射表和 WiFi 凭据。
 * 必须发生在任何 tunnel_wifi_connect 之前，否则凭据还没加载就去连网。 */
/* 开机/串口 info 命令打印：连接信息一页全览，忘了密码看这里（串口终端多为 ASCII，用英文） */
void tunnel_print_info(void)
{
    LOG_I("==============================================");
    LOG_I(" m61-tunnel %s  build %s %s", TUNNEL_FW_VERSION, __DATE__, __TIME__);
    LOG_I(" relay server : %s:%u", s_relay_host, (unsigned)s_relay_port);
    LOG_I(" token        : %s", s_token);
    LOG_I(" mgmt pin      : %s  (for web page)", s_pin);
    LOG_I(" wifi         : %s", s_wifi_ssid);
    LOG_I(" mgmt page    : http://<board-ip>/?pw=%s", s_pin);
    LOG_I("               (board ip shown after wifi got ip, or check router)");
    LOG_I(" shell cmds   : info=show this  apmode=start setup AP");
    LOG_I("==============================================");
}

void tunnel_init(void)
{
    static int inited = 0;
    if (inited) {
        return;
    }
    inited = 1;
    map_load();
    tunnel_print_info(); /* 开机自报家门 */
    s_mgmt_lock = xSemaphoreCreateMutex();
    xTaskCreate(ap_watchdog_task, "apwd", 512, NULL, 10, NULL);
    xTaskCreate(led_task, "led", 512, NULL, 9, NULL);
    /* 管理页开机即启动（监听所有接口）——不能等连上 WiFi 才起：
     * AP 配网模式恰恰是连不上 WiFi 的场景，管理页必须先于网络可用 */
    xTaskCreate(mgmt_server_task, "mgmt", 1024, NULL, 11, NULL);
}

void tunnel_start(void)
{
    tunnel_init();
    if (s_tunnel_started) {
        return;
    }
    s_tunnel_started = 1;
    xTaskCreate(tunnel_task, "tunnelctl", 1024, NULL, 12, NULL);
}
