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
#define CFG_WIFI_SSID      "bocan-tech-2.4G"
#define CFG_WIFI_PASS      "bocan@2022"
#define CFG_AUTO_WIFI_CONNECT 1

/* ---------- 中继服务器 ---------- */
#define CFG_RELAY_HOST     "47.99.112.251"
#define CFG_RELAY_CTRL_PORT 7000            /* 控制+数据（单端口三合一） */
#define CFG_RELAY_DATA_PORT 7000
#define CFG_TOKEN          "f422759e554d52b9ddc24d87dcff49f7"
#define CFG_MGMT_PIN       "m61pin"         /* 管理页密码（短好输）；隧道认证仍用上面长 token */
#define CFG_TUNNEL_ID      "m61-32s"

/* ---------- 多目标映射 ---------- */
#define CFG_MAX_TARGETS    8
#define CFG_MAX_GPIOS     8                /* 可配置的 GPIO 引脚数（管理页增删） */
#define CFG_MGMT_PORT      80              /* 板载管理页（局域网 http://板子IP/ 或经映射端口访问） */
#define CFG_CFG_FLASH_ADDR 0x3F3000        /* DATA 分区起始（bl616dk 4M 分区表，SDK 组件未占用） */


/* ---------- 硬件模块开关 ----------
 * 0 = 纯透传版（默认）：只有隧道/映射/SOCKS5/网络配置管理页
 * 1 = 硬件交互版：+ GPIO + ADC + PWM（构建时选择，运行时不可切） */
#define CFG_HW_MODULE     1

/* ---------- 硬件模块参数（CFG_HW_MODULE=1 时生效） ---------- */
#define CFG_MAX_PWMS      4                /* PWM 通道数 */
#define CFG_ADC_PIN       20               /* ADC 输入引脚（=ADC通道0，GPIO20） */
#define CFG_UART_BAUD     115200           /* 第二批：UART 透传波特率 */

/* ---------- 限额 ---------- */
#define CFG_MAX_TUNNELS    8               /* 并发连接槽位（v3: SOCKS5 模式浏览器并发多，提到8） */
#define CFG_BUF_SIZE       1024
#define CFG_IDLE_TIMEOUT_MS (90 * 1000)
#endif
