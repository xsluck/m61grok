#ifndef TUNNEL_H
#define TUNNEL_H

/* main() 里 board_init() 之后尽早调用：从 flash 加载映射表+WiFi 凭据（幂等） */
void tunnel_init(void);

/* 拿到 IP（CODE_WIFI_ON_GOT_IP）时调用一次（幂等）：起隧道任务 */
void tunnel_start(void);

/* GOT_IP 时调用：把当前实际连接的 WiFi 固化到 flash（串口手动连的也能记住） */
void tunnel_wifi_save_current(void);

/* 配网热点：开/关/查询（连不上 WiFi 时手机连热点配网） */
void tunnel_ap_start(void);
void tunnel_ap_stop(void);
int tunnel_ap_active(void);

/* 状态灯（main.c 的 WiFi 事件里调用） */
void tunnel_led_wifi(int ok);            /* 1=WiFi已连 0=未连(红常亮) */
void tunnel_led_wifi_connecting(int on); /* 1=正在连WiFi(绿闪) */

/* 管理页保存配置后调用：带着新映射表重连中继（tunnel.c 内部使用） */
void tunnel_apply_now(void);

/* 用 flash 里保存的 WiFi 凭据连接（main.c 自动连时调用） */
void tunnel_wifi_connect(void);

#endif
