/**
 * @file smart_config.c
 * @brief 配网实现 (AP 模式 + 手动输入 SSID/密码)
 *
 * 流程：
 *  1. 启动 SoftAP (Watchdog-AP / 12345678)
 *  2. 启动 HTTP 服务器 (端口 80)
 *  3. GET  /connect  -> 返回配网表单 HTML
 *     POST /connect  -> 解析表单 (ssid & password)，保存到 NVS，重启
 *  4. 重启后进入正常模式，读取 NVS 连接 WiFi
 *
 * 提交方式：application/x-www-form-urlencoded
 * 解析方式：httpd_query_key_value (最稳定，不依赖 JSON)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"

#include "smart_config.h"
#include "nvs_storage.h"
#include "wizard.html.h"

#define TAG "SMART_CFG"

// AP 配置
#define AP_SSID             "Watchdog-AP"
#define AP_PASSWORD         "12345678"
#define AP_CHANNEL          1
#define AP_MAX_CONNECTIONS  4
#define CONFIG_TIMEOUT_MS   (5 * 60 * 1000)  // 5 分钟超时

static httpd_handle_t s_server = NULL;
static smart_config_state_t s_state = SC_STATE_IDLE;

static void set_state(smart_config_state_t state)
{
    s_state = state;
    ESP_LOGI(TAG, "Config state: %d", state);
}

/* URL 解码 (application/x-www-form-urlencoded)。
 * 必要性: 前端用 encodeURIComponent() 编码提交, 而 httpd_query_key_value
 *         只做字符串匹配、不做解码。若不解码, WiFi 密码/SSID 里的
 *         & + % 空格 中文 等会以 %26 %2B %25 %20 形式存入 NVS,
 *         导致 WiFi 永远连不上。
 * 就地解码, 非法转义序列按原样保留。 */
static void url_decode(char *s)
{
    static const char hex[] = "0123456789abcdefABCDEF";
    size_t n = strlen(s), i = 0, o = 0;
    while (i < n) {
        if (s[i] == '+') {
            s[o++] = ' ';
            i++;
        } else if (s[i] == '%' && i + 2 < n &&
                   strchr(hex, s[i + 1]) && strchr(hex, s[i + 2])) {
            char hexbuf[3] = { s[i + 1], s[i + 2], '\0' };
            s[o++] = (char)strtol(hexbuf, NULL, 16);
            i += 3;
        } else {
            s[o++] = s[i++];
        }
    }
    s[o] = '\0';
}

// ========== HTTP 处理器 ==========

// GET /connect 或 GET /  -> 返回配网表单
static esp_err_t handler_get_page(httpd_req_t *req)
{
    const char *html = wizard_get_html();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

// POST /connect -> 解析表单，保存配置，重启
static esp_err_t handler_post_connect(httpd_req_t *req)
{
    // 读取 POST body
    char buf[512];
    int len = req->content_len;
    if (len > (int)sizeof(buf) - 1) {
        len = (int)sizeof(buf) - 1;
    }
    if (len > 0) {
        int r = httpd_req_recv(req, buf, len);
        if (r > 0) {
            buf[r] = '\0';

            // 解析表单字段: ssid=xxx&password=yyy
            char ssid[64] = {0};
            char password[64] = {0};
            httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
            httpd_query_key_value(buf, "password", password, sizeof(password));

            // 前端用 encodeURIComponent 编码, 这里必须解码 (见 url_decode 注释)
            url_decode(ssid);
            url_decode(password);

            ESP_LOGI(TAG, "Received SSID='%s', password_len=%d", ssid, (int)strlen(password));

            if (strlen(ssid) == 0) {
                const char *err = "{\"status\":\"error\",\"message\":\"请填写 WiFi 名称\"}";
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, err, strlen(err));
                return ESP_OK;
            }

            // 保存 SSID/密码到 NVS
            esp_err_t err = nvs_save_wifi_config(ssid, password);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_save_wifi_config failed: %s", esp_err_to_name(err));
                httpd_resp_send_500(req);
                return ESP_OK;
            }

            set_state(SC_STATE_SUCCESS);

            const char *resp = "{\"status\":\"ok\",\"message\":\"配置已保存，设备即将重启并连接 WiFi...\"}";
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, resp, strlen(resp));

            // 延迟重启，让响应先发出去
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
            return ESP_OK;
        }
    }

    httpd_resp_send_500(req);
    return ESP_OK;
}

// 处理 favicon.ico (避免 404 刷屏)
static esp_err_t handler_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// 启动 HTTP 服务器
static esp_err_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // GET / 和 GET /connect 都返回配网页面
    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_get_page,
    };
    httpd_uri_t uri_connect_get = {
        .uri = "/connect",
        .method = HTTP_GET,
        .handler = handler_get_page,
    };
    // POST /connect 处理表单提交
    httpd_uri_t uri_connect_post = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = handler_post_connect,
    };
    // 忽略 favicon
    httpd_uri_t uri_favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = handler_favicon,
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_connect_get);
    httpd_register_uri_handler(s_server, &uri_connect_post);
    httpd_register_uri_handler(s_server, &uri_favicon);

    ESP_LOGI(TAG, "Web server started on port 80");
    return ESP_OK;
}

// 启动 AP 模式
static esp_err_t start_ap_mode(void)
{
    // 确保先停掉可能已有的 WiFi
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (esp_netif_create_default_wifi_ap() == NULL) {
        ESP_LOGE(TAG, "Failed to create default netif for AP");
    }

    // 防御: 若 WiFi 驱动已初始化 (如 STA 连接失败后直接转配网), 跳过重复 init。
    // esp_wifi_init 对已初始化的驱动返回错误, 原来的 ESP_ERROR_CHECK 会直接 panic。
    wifi_mode_t cur_mode;
    if (esp_wifi_get_mode(&cur_mode) != ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    }

    wifi_config_t wifi_config = {0};
    strcpy((char *)wifi_config.ap.ssid, AP_SSID);
    strcpy((char *)wifi_config.ap.password, AP_PASSWORD);
    wifi_config.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    wifi_config.ap.channel = AP_CHANNEL;
    wifi_config.ap.max_connection = AP_MAX_CONNECTIONS;
    wifi_config.ap.authmode = (strlen(AP_PASSWORD) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID=%s, Password=%s", AP_SSID, AP_PASSWORD);
    return ESP_OK;
}

esp_err_t smart_config_start(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Config Mode (AP) Started");
    ESP_LOGI(TAG, "  AP: %s", AP_SSID);
    ESP_LOGI(TAG, "  Password: %s", AP_PASSWORD);
    ESP_LOGI(TAG, "  Connect WiFi and open: http://192.168.4.1");
    ESP_LOGI(TAG, "========================================");

    set_state(SC_STATE_AP_STARTED);

    esp_err_t ret = start_ap_mode();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AP mode");
        return ret;
    }

    set_state(SC_STATE_WEB_SERVER_STARTED);

    ret = start_web_server();
    if (ret != ESP_OK) {
        return ret;
    }

    // 等待配网完成 (在 handler_post_connect 中 esp_restart)
    ESP_LOGI(TAG, "Waiting for configuration... (timeout 5min)");

    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    while (1) {
        // 喂 Task WDT: 配网最长要等 5 分钟, 而 TWDT 超时只有 30s。
        // 若不喂狗, ESP32 会在用户还没填完 WiFi 密码时就 panic 重启。
        // (smart_config_start 由 system_task 调用, 因此这里喂的是 system_task)
        esp_task_wdt_reset();

        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (now_ms - start_ms > CONFIG_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Config timeout, restarting...");
            set_state(SC_STATE_TIMEOUT);
            esp_restart();
        }
    }

    return ESP_OK;  // 不会到这里 (esp_restart)
}

void smart_config_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

smart_config_state_t smart_config_get_state(void)
{
    return s_state;
}
