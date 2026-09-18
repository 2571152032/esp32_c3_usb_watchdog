/**
 * @file notify.c
 * @brief 事件通知推送实现 (HTTP/HTTPS GET Webhook)
 *
 * 设计要点:
 *  - 推送地址与开关保存在 NVS, 掉电不丢;
 *  - URL 支持 {TITLE} / {MSG} 占位符, 无占位符时自动追加查询参数;
 *  - 发送在独立任务中完成, 看门狗任务不会因网络超时被阻塞;
 *  - 使用 esp_crt_bundle 做服务端证书校验 (HTTPS 可用);
 *  - 同一时刻只允许一个发送任务, 避免告警风暴时耗尽内存。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "notify.h"
#include "nvs_storage.h"
#include "event_log.h"

#define TAG "NOTIFY"

#define NOTIFY_TASK_STACK       7168
#define NOTIFY_HTTP_TIMEOUT_MS  8000
#define NOTIFY_TITLE_MAX_LEN    64
#define NOTIFY_TEXT_MAX_LEN     192
// 百分号编码后最长为原文 3 倍 (每个字节最多变成 %XX)
#define NOTIFY_ENC_TITLE_MAX    ((NOTIFY_TITLE_MAX_LEN * 3) + 1)
#define NOTIFY_ENC_TEXT_MAX     ((NOTIFY_TEXT_MAX_LEN  * 3) + 1)
// URL 模板(192) + 标题编码(193) + 内容编码(577) 后仍在 1KB 以内
#define NOTIFY_URL_OUT_MAX_LEN  1024

typedef struct {
    char title[NOTIFY_TITLE_MAX_LEN];
    char text[NOTIFY_TEXT_MAX_LEN];
    char url[NOTIFY_URL_MAX_LEN];   // 入队时快照, 避免发送期间配置被改写
} notify_msg_t;

static struct {
    bool enabled;
    char url[NOTIFY_URL_MAX_LEN];
    volatile bool sending;   // 有发送任务在运行
} s_nt = {0};

// 保护 sending 标志的 check-then-set (Web 测试通知与宕机通知可能并发触发)
static portMUX_TYPE s_nt_mux = portMUX_INITIALIZER_UNLOCKED;

/* ==================== 内部工具 ==================== */

// RFC3986 百分号编码 (保留 unreserved 字符)
static void percent_encode(const char *in, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (out_size == 0) return;

    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
        unsigned char c = *p;
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            if (o + 1 >= out_size) break;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= out_size) break;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

// 把 buf 中所有 key 替换为 value; 返回是否发生过替换
// 注: value 已经过百分号编码, 不会再包含 key 本身, 不会死循环
static bool replace_placeholder(char *buf, size_t buf_size, const char *key, const char *value)
{
    size_t klen = strlen(key);
    size_t vlen = strlen(value);
    bool found = false;
    char *pos = buf;

    while ((pos = strstr(pos, key)) != NULL) {
        size_t used = (size_t)(pos - buf);
        size_t tail = strlen(pos + klen);
        if (used + vlen + tail + 1 > buf_size) break;   // 缓冲不足, 放弃剩余替换
        memmove(pos + vlen, pos + klen, tail + 1);
        memcpy(pos, value, vlen);
        pos += vlen;
        found = true;
    }
    return found;
}

/* ==================== 发送任务 ==================== */

static void notify_task(void *pvParameter)
{
    notify_msg_t *msg = (notify_msg_t *)pvParameter;

    char enc_title[NOTIFY_ENC_TITLE_MAX];
    char enc_text[NOTIFY_ENC_TEXT_MAX];
    percent_encode(msg->title, enc_title, sizeof(enc_title));
    percent_encode(msg->text,  enc_text,  sizeof(enc_text));

    char url[NOTIFY_URL_OUT_MAX_LEN];
    snprintf(url, sizeof(url), "%s", msg->url);

    bool has_ph = replace_placeholder(url, sizeof(url), "{TITLE}", enc_title);
    has_ph = replace_placeholder(url, sizeof(url), "{MSG}", enc_text) || has_ph;

    if (!has_ph) {
        // 未使用占位符: 自动追加查询参数, 保证至少能把内容带出去
        size_t len = strlen(url);
        if (len < sizeof(url) - 1) {
            snprintf(url + len, sizeof(url) - len, "%ctitle=%s&msg=%s",
                     (strchr(url, '?') != NULL) ? '&' : '?', enc_title, enc_text);
        }
    }

    ESP_LOGI(TAG, "Sending notification to: %s", url);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = NOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init http client");
        free(msg);
        s_nt.sending = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32C3-Watchdog/1.0");
    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Notification sent (HTTP %d)", status);
        LOG_I("通知推送成功 (HTTP %d)", status);
    } else {
        ESP_LOGW(TAG, "Notification failed: %s (HTTP %d)", esp_err_to_name(ret), status);
        LOG_W("通知推送失败: %s (HTTP %d)", esp_err_to_name(ret), status);
    }

    free(msg);
    s_nt.sending = false;
    vTaskDelete(NULL);
}

/* ==================== Public API ==================== */

esp_err_t notify_init(void)
{
    memset(&s_nt, 0, sizeof(s_nt));
    nvs_load_notify_config(&s_nt.enabled, s_nt.url, sizeof(s_nt.url));
    ESP_LOGI(TAG, "Notify init: enabled=%d, url=%s",
             (int)s_nt.enabled, s_nt.url[0] ? s_nt.url : "(not set)");
    return ESP_OK;
}

bool notify_is_enabled(void)
{
    return s_nt.enabled;
}

const char *notify_get_url(void)
{
    return s_nt.url;
}

esp_err_t notify_set_config(bool enabled, const char *url)
{
    s_nt.enabled = enabled;
    if (url) {
        snprintf(s_nt.url, sizeof(s_nt.url), "%s", url);
    }

    ESP_LOGI(TAG, "Notify config: enabled=%d, url=%s",
             (int)s_nt.enabled, s_nt.url[0] ? s_nt.url : "(not set)");
    return nvs_save_notify_config(s_nt.enabled, s_nt.url);
}

void notify_send_async(const char *title, const char *text)
{
    // 原子地完成 "检查 + 占位", 防止并发调用创建两个发送任务
    bool start = false;
    taskENTER_CRITICAL(&s_nt_mux);
    if (s_nt.enabled && s_nt.url[0] != '\0' && !s_nt.sending) {
        s_nt.sending = true;
        start = true;
    }
    taskEXIT_CRITICAL(&s_nt_mux);

    if (!start) {
        if (!s_nt.enabled) {
            ESP_LOGI(TAG, "Notification disabled, skip");
        } else if (s_nt.url[0] == '\0') {
            ESP_LOGW(TAG, "Notification URL is empty, skip");
        } else {
            ESP_LOGW(TAG, "Previous notification still sending, skip");
        }
        return;
    }

    notify_msg_t *msg = calloc(1, sizeof(notify_msg_t));
    if (!msg) {
        ESP_LOGE(TAG, "No memory for notification");
        s_nt.sending = false;
        return;
    }
    snprintf(msg->title, sizeof(msg->title), "%s", title ? title : "");
    snprintf(msg->text,  sizeof(msg->text),  "%s", text  ? text  : "");
    snprintf(msg->url,   sizeof(msg->url),   "%s", s_nt.url);

    if (xTaskCreate(notify_task, "notify_task", NOTIFY_TASK_STACK, msg, 4, NULL) != pdPASS) {
        s_nt.sending = false;
        free(msg);
        ESP_LOGE(TAG, "Failed to create notify task");
    }
}
