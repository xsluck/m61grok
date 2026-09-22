/*
 * tunnel_config.h —— m61-tunnel v2 板端配置
 *
 * v2 起支持多目标透传 + 板载 HTTP 管理页：
 *   - 映射表（公网访客端口 → 局域网目标）保存在 flash，管理页在线改，改完即时生效
 *   - 下面这张"默认表"只在首次启动或配置损坏时写入，之后以 flash 里的为准
 */
#ifndef TUNNEL_CONFIG_H
#define TUNNEL_CONFIG_H

/* ---------- WiFi ---------- */
#define CFG_WIFI_SSID      "your-wifi-ssid"   /* 也可以不填对，首次用配网页设置 */
#define CFG_WIFI_PASS      "your-wifi-password"
#define CFG_AUTO_WIFI_CONNECT 1

/* ---------- 中继服务器 ---------- */
#define CFG_RELAY_HOST     "your-vps-ip"     /* 也可以烧录后在管理页改 */
#define CFG_RELAY_CTRL_PORT 7000            /* 控制+数据（单端口三合一） */
#define CFG_RELAY_DATA_PORT 7000
#define CFG_TOKEN          "change-me-to-a-random-32-char-string"
#define CFG_MGMT_PIN       "m61pin"         /* 管理页密码（短好输）；隧道认证仍用上面长 token */
#define CFG_TUNNEL_ID      "m61-32s"

/* ---------- 多目标映射 ---------- */
#define CFG_MAX_TARGETS    8
#define CFG_MGMT_PORT      80              /* 板载管理页（局域网 http://板子IP/ 或经映射端口访问） */
#define CFG_CFG_FLASH_ADDR 0x3F3000        /* DATA 分区起始（bl616dk 4M 分区表，SDK 组件未占用） */

/* 目标 host 填 "local" 表示板载管理页 */
#define CFG_DEFAULT_MAP \
    { 21114, "192.168.1.100", 8080 },      /* 公网:21114 → 内网 192.168.1.100:8080 */ \
    { 21115, "192.168.1.100", 3000 },      /* 公网:21115 → 内网 192.168.1.100:3000 */ \
    { 21116, "local", CFG_MGMT_PORT }      /* 公网:21116 → 板载管理页 */

/* ---------- 限额 ---------- */
#define CFG_MAX_TUNNELS    4               /* 每个目标并发访客连接上限（全局共享槽位） */
#define CFG_BUF_SIZE       1024
#define CFG_IDLE_TIMEOUT_MS (90 * 1000)
#endif
