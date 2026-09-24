/**
 * @file notify.c
 * @brief 事件通知推送实现 (HTTP/HTTPS POST + Bearer 鉴权)
 *
 * 设计要点:
 *  - 开关 / 服务地址 / 令牌保存在 NVS, 掉电不丢;
 *  - 默认以 POST <BaseURL>/n + Authorization: Bearer <token> + JSON 发送;
 *    内容包含 event 事件类型, 便于自建程序按类型分支处理;
 *  - 兼容旧的 "GET + {TITLE}/{MSG} 占位符" Webhook: 地址含占位符时自动回退;
 *  - 发送在独立任务中完成, 看门狗任务不会因网络超时被阻塞;
 *  - 使用 esp_crt_bundle 做服务端证书校验 (HTTPS 可用);
 *  - 同一时刻只允许一个发送任务, 避免告警风暴时耗尽内存。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "notify.h"
#include "nvs_storage.h"
#include "event_log.h"

#define TAG "NOTIFY"

// HTTPS 握手 (mbedTLS + crt_bundle) 本身就要 ~10KB 栈,
// 之前 7168 会在 TLS 握手的 SHA512/HMAC 处栈溢出 (Stack protection fault),
// 因此这里给足 14KB, 避免"发 HTTPS 通知就重启"。
#define NOTIFY_TASK_STACK       14336
// 12s: 接收端往往要同步做转发 (如 SMTP 发信, 常见 10s 超时), 8s 容易提前断开;
// 仍小于 nginx 常见的 proxy_read_timeout (15s), 不会被网关截断
#define NOTIFY_HTTP_TIMEOUT_MS  12000
#define NOTIFY_TITLE_MAX_LEN    64
#define NOTIFY_TEXT_MAX_LEN     192
// JSON 最坏长度: 标题转义 128 + 内容转义 448 + 令牌转义 192 + 结构与时间戳 ≈ 100
#define NOTIFY_BODY_MAX_LEN     900
// 百分号编码后最长为原文 3 倍 (每个字节最多变成 %XX)
#define NOTIFY_ENC_TITLE_MAX    ((NOTIFY_TITLE_MAX_LEN * 3) + 1)
#define NOTIFY_ENC_TEXT_MAX     ((NOTIFY_TEXT_MAX_LEN  * 3) + 1)
// URL 模板(192) + 标题编码(193) + 内容编码(577) 后仍在 1KB 以内
#define NOTIFY_URL_OUT_MAX_LEN  1024
#define NOTIFY_QUERY_MAX_LEN    (NOTIFY_ENC_TITLE_MAX + NOTIFY_ENC_TEXT_MAX + 16)

typedef struct {
    char event[NOTIFY_EVENT_MAX_LEN];
    char title[NOTIFY_TITLE_MAX_LEN];
    char text[NOTIFY_TEXT_MAX_LEN];
    char url[NOTIFY_URL_MAX_LEN];     // 入队时快照, 避免发送期间配置被改写
    char token[NOTIFY_TOKEN_MAX_LEN];
} notify_msg_t;

static struct {
    bool enabled;
    char url[NOTIFY_URL_MAX_LEN];
    char token[NOTIFY_TOKEN_MAX_LEN];
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

// JSON 字符串转义 (仅处理 JSON 要求的字符; 非 ASCII 原样输出, 接收端按 UTF-8 解析)
static void json_escape(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    if (out_size == 0) return;
    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
        unsigned char c = *p;
        const char *rep = NULL;
        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default: break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (o + n + 1 > out_size) break;
            memcpy(out + o, rep, n);
            o += n;
        } else {
            if (c < 0x20) {
                if (o + 6 + 1 > out_size) break;
                o += (size_t)snprintf(out + o, out_size - o, "\\u%04x", c);
            } else {
                if (o + 2 > out_size) break;
                out[o++] = (char)c;
            }
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

// 把服务地址规整为 "…/n": 去掉结尾多余的 /, 若不以 /n 结尾则补上
static void build_notify_url(char *out, size_t out_size, const char *base)
{
    snprintf(out, out_size, "%s", base);
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    if (len >= 2 && strcmp(out + len - 2, "/n") == 0) {
        return;   // 已经是 /n 结尾, 不重复追加
    }
    snprintf(out + len, out_size - len, "/n");
}

// 清理配置值: 删除 CR/LF/TAB (令牌里若混进换行会破坏 Authorization 头,
// 服务端通常直接回 400), 并去掉首尾空格 (粘贴时最容易带进来)
static void sanitize_config_value(char *s)
{
    if (!s) return;

    size_t o = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p != '\r' && *p != '\n' && *p != '\t') {
            s[o++] = *p;
        }
    }
    s[o] = '\0';

    size_t start = 0;
    while (s[start] == ' ') start++;
    if (start > 0) {
        memmove(s, s + start, strlen(s + start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') {
        s[--len] = '\0';
    }
}

/* ==================== 发送任务 ==================== */

// 发送过程用到的大缓冲 (~4KB): 放在堆上, 尽量少占任务栈
// (栈要留给 mbedTLS 握手, 否则 HTTPS 会栈溢出)
typedef struct {
    char url[NOTIFY_URL_OUT_MAX_LEN];
    char body[NOTIFY_BODY_MAX_LEN];
    char resp[256];
    char enc_title[NOTIFY_ENC_TITLE_MAX];
    char enc_text[NOTIFY_ENC_TEXT_MAX];
    char esc_title[NOTIFY_TITLE_MAX_LEN * 2];
    char esc_text[NOTIFY_TEXT_MAX_LEN * 2 + 64];
    char esc_token[NOTIFY_TOKEN_MAX_LEN * 2];
    char query[NOTIFY_QUERY_MAX_LEN];
} notify_scratch_t;

static void notify_task(void *pvParameter)
{
    notify_msg_t *msg = (notify_msg_t *)pvParameter;

    // 旧版 Webhook 兼容: 地址里仍带 {TITLE}/{MSG} 占位符 -> 走原来的 GET 方式
    bool legacy = (strstr(msg->url, "{TITLE}") != NULL) || (strstr(msg->url, "{MSG}") != NULL);

    notify_scratch_t *sc = calloc(1, sizeof(notify_scratch_t));
    if (!sc) {
        ESP_LOGE(TAG, "No memory for notify scratch buffer");
        free(msg);
        s_nt.sending = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_handle_t client = NULL;
    esp_err_t ret;
    int status = 0;

    if (legacy) {
        char *enc_title = sc->enc_title;
        char *enc_text  = sc->enc_text;
        percent_encode(msg->title, enc_title, NOTIFY_ENC_TITLE_MAX);
        percent_encode(msg->text,  enc_text,  NOTIFY_ENC_TEXT_MAX);

        snprintf(sc->url, sizeof(sc->url), "%s", msg->url);
        bool has_ph = replace_placeholder(sc->url, sizeof(sc->url), "{TITLE}", enc_title);
        has_ph = replace_placeholder(sc->url, sizeof(sc->url), "{MSG}", enc_text) || has_ph;
        if (!has_ph) {
            // 先拼查询串再手工追加, 避免 snprintf 写入 url 偏移位置时的截断告警
            snprintf(sc->query, sizeof(sc->query), "%ctitle=%s&msg=%s",
                     (strchr(sc->url, '?') != NULL) ? '&' : '?', enc_title, enc_text);
            size_t len = strlen(sc->url);
            size_t qlen = strlen(sc->query);
            if (len + qlen + 1 > sizeof(sc->url)) {
                qlen = sizeof(sc->url) - len - 1;
            }
            memcpy(sc->url + len, sc->query, qlen);
            sc->url[len + qlen] = '\0';
        }
        ESP_LOGI(TAG, "Sending legacy notification (GET): %s", sc->url);
    } else {
        char *esc_title = sc->esc_title;
        char *esc_text  = sc->esc_text;
        json_escape(msg->title, esc_title, sizeof(sc->esc_title));
        json_escape(msg->text,  esc_text,  sizeof(sc->esc_text));

        build_notify_url(sc->url, sizeof(sc->url), msg->url);
        long long ts = (long long)(event_log_time_synced() ? time(NULL) : 0);
        const char *ev = msg->event[0] ? msg->event : NOTIFY_EVENT_GENERIC;

        // 自建服务 (Flask/Go 等) 常见字段名: t=标题, m=内容, k=令牌。
        // 这里按 t/m/k 输出 (m 为空会被判 400 "empty message"),
        // 并附带 event / ts 便于接收端按类型分支与记录时间, 未知字段服务端会忽略。
        json_escape(msg->token, sc->esc_token, sizeof(sc->esc_token));
        snprintf(sc->body, sizeof(sc->body),
                 "{\"t\":\"%s\",\"m\":\"%s\",\"k\":\"%s\",\"event\":\"%s\",\"ts\":%lld}",
                 esc_title, esc_text, sc->esc_token, ev, ts);

        ESP_LOGI(TAG, "Sending notification: POST %s (event=%s)", sc->url, ev);
    }

    esp_http_client_config_t cfg = {
        .url = sc->url,
        .method = legacy ? HTTP_METHOD_GET : HTTP_METHOD_POST,
        .timeout_ms = NOTIFY_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init http client");
        free(sc);
        free(msg);
        s_nt.sending = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "User-Agent", "ESP32C3-Watchdog/1.0");

    size_t body_len = 0;
    if (!legacy) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        if (msg->token[0] != '\0') {
            // 默认按 "Bearer <token>" 发送: 多数自建服务 (Flask/Go) 只认这种写法,
            // 直接发原始令牌会被判 403。若令牌自带方案名则原样发送。
            char auth[NOTIFY_TOKEN_MAX_LEN + 16];
            if (strncmp(msg->token, "Bearer ", 7) == 0 ||
                strncmp(msg->token, "Token ",  6) == 0) {
                snprintf(auth, sizeof(auth), "%s", msg->token);
            } else {
                snprintf(auth, sizeof(auth), "Bearer %s", msg->token);
            }
            esp_http_client_set_header(client, "Authorization", auth);
            // 兼容只认 X-Auth-Token 的接收端 (同时提供, 不影响 Bearer 校验)
            esp_http_client_set_header(client, "X-Auth-Token", msg->token);
        } else {
            ESP_LOGW(TAG, "Notification token is empty, sending without Authorization");
        }
        body_len = strlen(sc->body);
        // 排错用: 打印实际发出的请求 (URL / 头 / body)
        ESP_LOGI(TAG, "POST %s | Authorization: %s | body: %s",
                 sc->url, msg->token[0] ? "Bearer ***" : "(none)", sc->body);
    } else {
        ESP_LOGI(TAG, "GET %s", sc->url);
    }

    // 用流式 API 发请求: 失败时可以读出服务端返回的 body (400/401 的具体原因)
    ret = esp_http_client_open(client, (int)body_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        free(sc);
        free(msg);
        s_nt.sending = false;
        vTaskDelete(NULL);
        return;
    }

    if (body_len > 0) {
        int w = esp_http_client_write(client, sc->body, (int)body_len);
        if (w < 0) {
            ESP_LOGW(TAG, "Failed to write POST body (%d)", w);
        }
    }

    int content_len = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);

    // 读取服务端响应 (最多 255 字节, 仅用于排错)
    char *resp = sc->resp;
    int r = esp_http_client_read(client, resp, 255);
    if (r > 0) {
        resp[r] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    (void)content_len;

    if (status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Notification sent (HTTP %d)", status);
        LOG_I("通知推送成功 (HTTP %d)", status);
    } else {
        // 打印服务端返回内容: 400 通常是服务端校验失败 (字段/格式/鉴权), 看这里最直接
        // 注: 这里不能打印 esp_err_to_name(ret) —— ret 是 esp_http_client_open() 的
        //     返回值, 请求发出成功时它就是 ESP_OK, 会让人误以为"发送成功了却失败"。
        ESP_LOGW(TAG, "Notification failed: HTTP %d, response: %s",
                 status, resp[0] ? resp : "(empty)");
        char snippet[120];
        snprintf(snippet, sizeof(snippet), "HTTP %d: %.90s", status, resp[0] ? resp : "(empty)");
        LOG_W("通知推送失败: %s", snippet);
    }

    free(sc);
    free(msg);
    s_nt.sending = false;
    vTaskDelete(NULL);
}

/* ==================== Public API ==================== */

esp_err_t notify_init(void)
{
    memset(&s_nt, 0, sizeof(s_nt));
    nvs_load_notify_config(&s_nt.enabled, s_nt.url, sizeof(s_nt.url),
                           s_nt.token, sizeof(s_nt.token));
    ESP_LOGI(TAG, "Notify init: enabled=%d, url=%s, token=%s",
             (int)s_nt.enabled, s_nt.url[0] ? s_nt.url : "(not set)",
             s_nt.token[0] ? "(set)" : "(not set)");
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

const char *notify_get_token(void)
{
    return s_nt.token;
}

esp_err_t notify_set_config(bool enabled, const char *url, const char *token)
{
    s_nt.enabled = enabled;
    if (url) {
        snprintf(s_nt.url, sizeof(s_nt.url), "%s", url);
        sanitize_config_value(s_nt.url);
    }
    if (token) {
        snprintf(s_nt.token, sizeof(s_nt.token), "%s", token);
        sanitize_config_value(s_nt.token);
    }

    ESP_LOGI(TAG, "Notify config: enabled=%d, url=%s, token=%s",
             (int)s_nt.enabled, s_nt.url[0] ? s_nt.url : "(not set)",
             s_nt.token[0] ? "(set)" : "(not set)");
    return nvs_save_notify_config(s_nt.enabled, s_nt.url, s_nt.token);
}

void notify_send_event(const char *event, const char *title, const char *text)
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
    snprintf(msg->event, sizeof(msg->event), "%s",
             (event && event[0]) ? event : NOTIFY_EVENT_GENERIC);
    snprintf(msg->title, sizeof(msg->title), "%s", title ? title : "");
    snprintf(msg->text,  sizeof(msg->text),  "%s", text  ? text  : "");
    snprintf(msg->url,   sizeof(msg->url),   "%s", s_nt.url);
    snprintf(msg->token, sizeof(msg->token), "%s", s_nt.token);

    if (xTaskCreate(notify_task, "notify_task", NOTIFY_TASK_STACK, msg, 4, NULL) != pdPASS) {
        s_nt.sending = false;
        free(msg);
        ESP_LOGE(TAG, "Failed to create notify task");
    }
}

void notify_send_async(const char *title, const char *text)
{
    notify_send_event(NOTIFY_EVENT_GENERIC, title, text);
}
