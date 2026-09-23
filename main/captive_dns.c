/**
 * @file captive_dns.c
 * @brief 极简 DNS 服务器: 所有 A 查询一律回答 AP 自身地址
 *
 * 只实现需要的部分:
 *  - 只处理标准查询 (QR=0), 且问题数为 1;
 *  - A 记录 (QTYPE=1, IN) 回答 AP 地址; AAAA 等其它类型回 NOERROR + 0 条应答,
 *    让客户端退回 IPv4;
 *  - 不递归、不做缓存 —— 配网阶段够用, 且代码量小、出错面小。
 */

#include <string.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "captive_dns.h"

static const char *TAG = "CAP_DNS";

#define DNS_PORT         53
#define DNS_BUF_SIZE     512
#define DNS_TASK_STACK   3072
#define DNS_TTL_S        60
#define DNS_RECV_TO_MS   500     // 接收超时: 保证 stop 时任务能自己退出并关闭 socket

static int            s_sock     = -1;
static volatile bool  s_running  = false;
static TaskHandle_t   s_task     = NULL;
static uint32_t       s_ap_ip    = 0;    // 网络字节序

// 跳过 DNS 名字段 (普通标签 或 压缩指针), 返回名字段结束偏移; 解析失败返回 0
static size_t dns_skip_name(const uint8_t *p, size_t len, size_t off)
{
    while (off < len) {
        uint8_t l = p[off];
        if (l == 0) {
            return off + 1;                 // 名字结束
        }
        if ((l & 0xC0) == 0xC0) {
            return (off + 2 <= len) ? (off + 2) : 0;   // 压缩指针
        }
        if (l > 63) {
            return 0;                       // 非法标签长度
        }
        off += l + 1;
    }
    return 0;
}

// 构造应答; 返回应答长度, 0 表示不应答 (非查询 / 格式非法)
static size_t dns_build_reply(const uint8_t *req, size_t req_len, uint8_t *out, size_t out_max)
{
    if (req_len < 12) return 0;

    uint16_t flags   = ((uint16_t)req[2] << 8) | req[3];
    uint16_t qdcount = ((uint16_t)req[4] << 8) | req[5];

    if (flags & 0x8000) return 0;      // 是应答, 不是查询
    if (qdcount != 1)   return 0;      // 只处理单问题查询

    size_t qend = dns_skip_name(req, req_len, 12);
    if (qend == 0 || qend + 4 > req_len) return 0;

    uint16_t qtype  = ((uint16_t)req[qend]     << 8) | req[qend + 1];
    uint16_t qclass = ((uint16_t)req[qend + 2] << 8) | req[qend + 3];
    size_t   qlen   = qend + 4 - 12;   // 问题段 (名字 + QTYPE + QCLASS) 长度

    // A + IN 才给地址; 其它类型 (如 AAAA) 回空应答
    uint16_t ancount = (qtype == 1 && qclass == 1) ? 1 : 0;

    size_t o = 0;
    memset(out, 0, 12);
    out[0] = req[0];                   // 事务 ID
    out[1] = req[1];

    // QR=1, Opcode=0, AA=1, RD 原样带回, RA=1, RCODE=0
    uint16_t rflags = (uint16_t)(0x8000 | 0x0400 | 0x0080 | (flags & 0x0100));
    out[2] = (uint8_t)(rflags >> 8);
    out[3] = (uint8_t)(rflags & 0xFF);
    out[4] = 0;  out[5] = 1;                        // QDCOUNT = 1
    out[6] = (uint8_t)(ancount >> 8);
    out[7] = (uint8_t)(ancount & 0xFF);             // ANCOUNT
    out[8] = 0;  out[9]  = 0;                       // NSCOUNT
    out[10] = 0; out[11] = 0;                       // ARCOUNT
    o = 12;

    if (o + qlen > out_max) return 0;
    memcpy(out + o, req + 12, qlen);                // 原样回写问题段
    o += qlen;

    if (ancount) {
        if (o + 16 > out_max) return 0;
        out[o++] = 0xC0; out[o++] = 0x0C;           // 名字指针 -> 偏移 12
        out[o++] = 0;    out[o++] = 1;              // TYPE  = A
        out[o++] = 0;    out[o++] = 1;              // CLASS = IN
        out[o++] = 0;    out[o++] = 0;
        out[o++] = 0;    out[o++] = (uint8_t)DNS_TTL_S;   // TTL
        out[o++] = 0;    out[o++] = 4;              // RDLENGTH = 4
        memcpy(out + o, &s_ap_ip, 4);               // RDATA (已是网络字节序)
        o += 4;
    }
    return o;
}

static void dns_task(void *pvParameter)
{
    uint8_t req[DNS_BUF_SIZE];
    uint8_t resp[DNS_BUF_SIZE];

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        s_task = NULL;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(DNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind UDP port %d", DNS_PORT);
        close(s_sock);
        s_sock = -1;
        s_task = NULL;
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    // 接收超时: 让 while 循环能周期性检查 s_running, 退出时才关得掉 socket
    struct timeval tv;
    tv.tv_sec  = DNS_RECV_TO_MS / 1000;
    tv.tv_usec = (DNS_RECV_TO_MS % 1000) * 1000;
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Captive portal DNS server started (answer all A queries -> AP)");

    while (s_running) {
        struct sockaddr_storage src;
        socklen_t srclen = sizeof(src);
        int len = recvfrom(s_sock, req, sizeof(req), 0, (struct sockaddr *)&src, &srclen);
        if (len < 12) {
            continue;   // 超时或包太短
        }

        size_t rlen = dns_build_reply(req, (size_t)len, resp, sizeof(resp));
        if (rlen > 0) {
            sendto(s_sock, resp, rlen, 0, (struct sockaddr *)&src, srclen);
        }
    }

    close(s_sock);
    s_sock = -1;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(uint32_t ap_ip_addr)
{
    if (s_running) return ESP_OK;

    s_ap_ip   = ap_ip_addr;
    s_running = true;

    if (xTaskCreate(dns_task, "cap_dns", DNS_TASK_STACK, NULL, 4, &s_task) != pdPASS) {
        s_running = false;
        s_task    = NULL;
        ESP_LOGE(TAG, "Failed to create DNS task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void captive_dns_stop(void)
{
    if (!s_running) return;

    s_running = false;
    for (int i = 0; i < 20 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_task) {
        vTaskDelete(s_task);   // 兜底
        s_task = NULL;
    }
    ESP_LOGI(TAG, "Captive portal DNS server stopped");
}
