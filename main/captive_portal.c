/**
 * @file captive_portal.c
 * @brief 强制门户 DNS 劫持实现
 *
 * 实现一个极简 DNS 服务器:
 *   - 监听 UDP 53
 *   - 所有 A 记录查询 -> 回答 AP 自身 IP
 *   - AAAA / 其它类型 -> NOERROR + 空回答 (让客户端回落到 IPv4)
 *
 * 为什么 AAAA 不能也回答 A 记录:
 *   手机通常先查 AAAA。若对 AAAA 查询回一条 A 记录, 客户端会拿到
 *   长度/类型不匹配的资源记录, 直接判定解析失败, 反而不走 HTTP 探测。
 *   正确做法是回 "NOERROR + 0 条回答", 客户端随后会查 A 记录。
 *
 * 内存约定: 单个 DNS 报文 512 字节, 直接放栈上 (远小于 256 字节阈值? 不,
 *   512 > 256, 故报文缓冲放在任务栈但任务是 3072 字节, 只占 1/6, 安全;
 *   且任务栈与 httpd 栈互不影响)。
 */

#include "captive_portal.h"

#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define TAG "CAPTIVE"

#define DNS_PORT        53
#define DNS_BUF_SIZE    512
#define DNS_TTL         60          /* 秒, 短一些, 避免客户端长期缓存假地址 */
#define DNS_TASK_STACK  3072
#define DNS_TASK_PRIO   5

#define DNS_HDR_LEN     12
#define DNS_QTYPE_A     1
#define DNS_QTYPE_AAAA  28
#define DNS_QCLASS_IN   1

#define AP_IFKEY        "WIFI_AP_DEF"
#define DEFAULT_AP_IP   "192.168.4.1"

static volatile TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static uint32_t s_ap_ip = 0;        /* 网络字节序 */

/* ------------------------------------------------------------------ */
/* DNS 报文解析 / 应答                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief 解析一条查询并就地改写为应答后发回
 * @param sock    UDP socket
 * @param buf     收到的完整报文
 * @param len     报文长度
 * @param cli     客户端地址
 * @param cli_len 地址结构长度
 */
static void dns_handle_query(int sock, uint8_t *buf, int len,
                             const struct sockaddr_in *cli, socklen_t cli_len)
{
    if (len < DNS_HDR_LEN) {
        return;
    }
    /* bit15 = QR, 1 表示这是应答而非查询, 忽略 */
    if (buf[2] & 0x80) {
        return;
    }
    /* 只处理单问题报文 (探测请求都是单问题) */
    uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
    if (qdcount != 1) {
        return;
    }

    /* 走到 QNAME 结尾 (0x00 结束符), 不支持压缩指针 (查询里不会出现) */
    int i = DNS_HDR_LEN;
    while (i < len && buf[i] != 0) {
        if ((buf[i] & 0xC0) != 0) {     /* 0xC0 = 压缩指针 */
            return;
        }
        i += buf[i] + 1;
    }
    if (i >= len || buf[i] != 0) {
        return;
    }
    i++;                                /* 跳过 QNAME 结束符 */
    if (i + 4 > len) {
        return;                         /* QTYPE/QCLASS 不完整 */
    }

    uint16_t qtype  = (uint16_t)((buf[i] << 8) | buf[i + 1]);
    uint16_t qclass = (uint16_t)((buf[i + 2] << 8) | buf[i + 3]);
    int q_end = i + 4;                  /* 问题区结束位置 */

    int ancount = (qtype == DNS_QTYPE_A && qclass == DNS_QCLASS_IN) ? 1 : 0;
    int resp_len = q_end;

    /* ---- 改写头部 ----
     * buf[2]: QR(0x80) OPCODE AA(0x04) TC(0x02) RD(0x01)
     * buf[3]: RA(0x80) Z AD CD RCODE
     */
    buf[2] = (uint8_t)(0x84 | (buf[2] & 0x01));   /* QR=1, AA=1, 保留原 RD */
    buf[3] = 0x80;                                /* RA=1, RCODE=0 (NOERROR) */
    buf[4] = 0;   buf[5] = 1;                     /* QDCOUNT = 1 */
    buf[6] = 0;   buf[7] = (uint8_t)ancount;      /* ANCOUNT  */
    buf[8] = 0;   buf[9]  = 0;                    /* NSCOUNT  */
    buf[10] = 0;  buf[11] = 0;                    /* ARCOUNT  */

    if (ancount) {
        if (q_end + 16 > DNS_BUF_SIZE) {
            return;
        }
        uint8_t *p = buf + q_end;
        p[0] = 0xC0; p[1] = 0x0C;       /* NAME: 指向偏移 12 的 QNAME */
        p[2] = 0x00; p[3] = 0x01;       /* TYPE  = A   */
        p[4] = 0x00; p[5] = 0x01;       /* CLASS = IN  */
        p[6] = 0x00; p[7] = 0x00;
        p[8] = 0x00; p[9] = DNS_TTL;    /* TTL   */
        p[10] = 0x00; p[11] = 0x04;     /* RDLENGTH = 4 */
        memcpy(p + 12, &s_ap_ip, 4);    /* RDATA = AP IP (网络序) */
        resp_len = q_end + 16;
    }

    sendto(sock, buf, resp_len, 0, (const struct sockaddr *)cli, cli_len);
}

/* ------------------------------------------------------------------ */
/* DNS 任务                                                            */
/* ------------------------------------------------------------------ */

static void dns_task(void *pvParameter)
{
    (void)pvParameter;
    uint8_t buf[DNS_BUF_SIZE];
    struct sockaddr_in srv;
    struct sockaddr_in cli;
    socklen_t cli_len;
    int sock;

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port = htons(DNS_PORT);

    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        ESP_LOGE(TAG, "bind udp:53 failed (port busy?)");
        close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* 接收超时: 让 stop() 后任务能在 1s 内自然退出, 而不是永远卡在 recvfrom */
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t a = s_ap_ip;
    ESP_LOGI(TAG, "DNS hijack started: * -> %u.%u.%u.%u",
             (unsigned)(a & 0xFF), (unsigned)((a >> 8) & 0xFF),
             (unsigned)((a >> 16) & 0xFF), (unsigned)((a >> 24) & 0xFF));

    while (s_running) {
        cli_len = sizeof(cli);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&cli, &cli_len);
        if (len <= 0) {
            continue;                   /* 超时或出错, 继续等 */
        }
        dns_handle_query(sock, buf, len, &cli, cli_len);
    }

    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

uint32_t captive_portal_get_ap_ip(void)
{
    return s_ap_ip;
}

esp_err_t captive_portal_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey(AP_IFKEY);

    if (ap) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(ap, &info) == ESP_OK && info.ip.addr != 0) {
            s_ap_ip = info.ip.addr;
        }
    }
    if (s_ap_ip == 0) {
        s_ap_ip = inet_addr(DEFAULT_AP_IP);
    }

    /* 把 DHCP 下发给客户端的 DNS 指向设备自己。
     * 部分 IDF 版本默认不下发 option 6, 客户端就会用运营商 DNS 去查
     * 真实域名, 从而绕过我们的劫持 —— 这里显式设一次兜底。 */
    if (ap) {
        esp_netif_dns_info_t dns;
        memset(&dns, 0, sizeof(dns));
        dns.ip.type = IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = s_ap_ip;
        esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    }

    TaskHandle_t h = NULL;
    s_running = true;
    BaseType_t ok = xTaskCreate(dns_task, "captive_dns", DNS_TASK_STACK,
                                NULL, DNS_TASK_PRIO, &h);
    s_task = h;
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        ESP_LOGE(TAG, "Failed to create DNS task (out of memory)");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void captive_portal_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    /* 等任务自己退出 (recv 超时 1s, 这里最多等 3s)。
     * 不能在别的任务里 close socket —— 那样任务会在已关闭的 fd 上再 close 一次。 */
    for (int i = 0; i < 300 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_task = NULL;
}
