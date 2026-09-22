/*
 * main.c —— m61-tunnel 板端入口（适配 gitee 版 bouffalo_sdk / aithinker Ai-M6X SDK）
 * 骨架抄自官方 examples/wifi/sta/wifi_tcp，改动三处（搜 m61-tunnel:）：
 *   1. CODE_WIFI_ON_MGMR_DONE 后自动连 WiFi（wifi_sta_connect C API）
 *   2. CODE_WIFI_ON_GOT_IP 后启动隧道任务
 *   3. 其余与官方示例一致
 */

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "mem.h"

#include <lwip/tcpip.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/netif.h>
#include <lwip/ip4_addr.h>

#include "bl_fw_api.h"
#include "wifi_mgmr_ext.h"
#include "wifi_mgmr.h"

#include "bflb_irq.h"
#include "bflb_uart.h"

#include "bl616_glb.h"
#include "rfparam_adapter.h"

#include "board.h"
#include "shell.h"

/* m61-tunnel: 板端隧道 */
#include "tunnel.h"
#include "tunnel_config.h"

#define DBG_TAG "MAIN"
#include "log.h"

struct bflb_device_s *gpio;

#define WIFI_STACK_SIZE  (1536)
#define TASK_PRIORITY_FW (16)

static struct bflb_device_s *uart0;

static TaskHandle_t wifi_fw_task;

static wifi_conf_t conf = {
    .country_code = "CN",
};

extern void shell_init_with_task(struct bflb_device_s *shell);

/* m61-tunnel: 板载 LED 状态 */
#include "tunnel.h"

/* m61-tunnel: 列出所有网络接口的 name/IP（AP 配网模式下找配置页地址用）。
 * 注意 ip4addr_ntoa 用静态缓冲，一次调用会被下一次覆盖，必须用 _r 版本各自缓冲 */
static void netif_dump_task(void *arg)
{
    (void)arg;
    vTaskDelay(3000 / portTICK_PERIOD_MS);

    /* 兜底：AP 接口（链表头部第一个 wifi 接口）没 IP 就设成 192.168.1.1
     * （正常路径 dhcpd_start 会按 DHCPD_SERVER_IP 宏自动设，这里双保险） */
    {
        struct netif *n = netif_list;
        if (n && n->name[0] == 'w') {
            if (n->ip_addr.addr == 0) {
                ip4_addr_t ip, nm, gw;
                IP4_ADDR(&ip, 192, 168, 1, 1);
                IP4_ADDR(&nm, 255, 255, 255, 0);
                IP4_ADDR(&gw, 192, 168, 1, 1);
                netif_set_addr(n, &ip, &nm, &gw);
                LOG_I("[NETIF] ap iface %c%c%d -> set 192.168.1.1\r\n",
                      n->name[0], n->name[1], n->num);
            }
        }
    }

    for (struct netif *n = netif_list; n != NULL; n = n->next) {
        char bufa[16], bufb[16];
        LOG_I("[NETIF] %c%c%d ip=%s mask=%s\r\n",
              n->name[0], n->name[1], n->num,
              ip4addr_ntoa_r(&n->ip_addr, bufa, sizeof(bufa)),
              ip4addr_ntoa_r(&n->netmask, bufb, sizeof(bufb)));
    }
    vTaskDelete(NULL);
}

/* m61-tunnel: 拿到 WiFi 协议栈就绪事件后自动连 WiFi（凭据在 flash，管理页可改） */
static void wifi_auto_connect_task(void *param)
{
    (void)param;
    vTaskDelay(500 / portTICK_PERIOD_MS);
    tunnel_wifi_connect();
    vTaskDelete(NULL);
}

int wifi_start_firmware_task(void)
{
    LOG_I("Starting wifi ...\r\n");

    /* enable wifi clock */
    GLB_PER_Clock_UnGate(GLB_AHB_CLOCK_IP_WIFI_PHY | GLB_AHB_CLOCK_IP_WIFI_MAC_PHY | GLB_AHB_CLOCK_IP_WIFI_PLATFORM);
    GLB_AHB_MCU_Software_Reset(GLB_AHB_MCU_SW_WIFI);

    /* set ble controller EM Size */
    GLB_Set_EM_Sel(GLB_WRAM160KB_EM0KB);

    if (0 != rfparam_init(0, NULL, 0)) {
        LOG_I("PHY RF init failed!\r\n");
        return 0;
    }

    LOG_I("PHY RF init success!\r\n");

    /* Enable wifi irq */
    extern void interrupt0_handler(void);
    bflb_irq_attach(WIFI_IRQn, (irq_callback)interrupt0_handler, NULL);
    bflb_irq_enable(WIFI_IRQn);

    xTaskCreate(wifi_main, (char *)"fw", WIFI_STACK_SIZE, NULL, TASK_PRIORITY_FW, &wifi_fw_task);

    return 0;
}

void wifi_event_handler(uint32_t code)
{
    switch (code) {
        case CODE_WIFI_ON_INIT_DONE: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_INIT_DONE\r\n", __func__);
            wifi_mgmr_init(&conf);
        } break;
        case CODE_WIFI_ON_MGMR_DONE: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_MGMR_DONE\r\n", __func__);
#if CFG_AUTO_WIFI_CONNECT /* m61-tunnel: 自动连 WiFi */
            {
                static int once = 0;
                if (!once) {
                    once = 1;
                    xTaskCreate(wifi_auto_connect_task, "autojoin", 1024, NULL, 10, NULL);
                }
            }
#endif
        } break;
        case CODE_WIFI_ON_SCAN_DONE: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_SCAN_DONE\r\n", __func__);
            wifi_mgmr_sta_scanlist();
        } break;
        case CODE_WIFI_ON_CONNECTED: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_CONNECTED\r\n", __func__);
            void mm_sec_keydump();
            mm_sec_keydump();
        } break;
        case CODE_WIFI_ON_GOT_IP: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_GOT_IP\r\n", __func__);
            LOG_I("[SYS] Memory left is %d Bytes\r\n", kfree_size());
            tunnel_led_wifi(1); /* 红灯灭：WiFi 已连 */
            {
                /* 打印板子 IP，方便局域网直接访问管理页 http://板子IP/?pw=... */
                uint32_t a, m, g, d;
                if (wifi_sta_ip4_addr_get(&a, &m, &g, &d) == 0) {
                    LOG_I("m61-tunnel: board IP %u.%u.%u.%u\r\n",
                          (unsigned)(a & 0xFF), (unsigned)((a >> 8) & 0xFF),
                          (unsigned)((a >> 16) & 0xFF), (unsigned)((a >> 24) & 0xFF));
                }
            }
            LOG_I("m61-tunnel: starting tunnel to %s\r\n", CFG_RELAY_HOST);
            tunnel_wifi_save_current(); /* m61-tunnel: 固化当前 WiFi（含串口手动连的） */
            /* 热点关闭由看门狗任务检测联网后自动执行（事件上下文不能直接调） */
            tunnel_start();             /* m61-tunnel: 拿到 IP，起隧道 */
        } break;
        case CODE_WIFI_ON_DISCONNECT: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_DISCONNECT\r\n", __func__);
            tunnel_led_wifi(0);            /* 红灯常亮：WiFi 断了 */
            tunnel_led_wifi_connecting(0); /* 绿灯灭 */
        } break;
        case CODE_WIFI_ON_AP_STARTED: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_AP_STARTED\r\n", __func__);
            /* 诊断：3 秒后列出所有网络接口和 IP（配网页地址从这里看） */
            {
                static int once = 0;
                if (!once) {
                    once = 1;
                    xTaskCreate((void (*)(void *))netif_dump_task, "netifdump", 512, NULL, 9, NULL);
                }
            }
        } break;
        case CODE_WIFI_ON_AP_STOPPED: {
            LOG_I("[APP] [EVT] %s, CODE_WIFI_ON_AP_STOPPED\r\n", __func__);
        } break;
        case CODE_WIFI_ON_AP_STA_ADD: {
            LOG_I("[APP] [EVT] [AP] [ADD] %lld\r\n", xTaskGetTickCount());
        } break;
        case CODE_WIFI_ON_AP_STA_DEL: {
            LOG_I("[APP] [EVT] [AP] [DEL] %lld\r\n", xTaskGetTickCount());
        } break;
        default: {
            LOG_I("[APP] [EVT] Unknown code %u \r\n", code);
        }
    }
}

int main(void)
{
    board_init();

    /* m61-tunnel: 先从 flash 加载配置（映射表+WiFi），再干别的 */
    tunnel_init();

    uart0 = bflb_device_get_by_name("uart0");
    shell_init_with_task(uart0);

    tcpip_init(NULL, NULL);
    wifi_start_firmware_task();

    vTaskStartScheduler();

    while (1) {
    }
}
