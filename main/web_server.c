#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "web_server.h"
#include "watchdog.h"
#include "usb_device.h"
#include "gpio_control.h"
#include "nvs_storage.h"
#include "ota_update.h"
#include "event_log.h"
#include "uptime.h"
#include "notify.h"
#include "dashboard_html.h"

static const char *TAG = "WEB";

static httpd_handle_t s_server = NULL;

// ==================== 会话管理 (简易 cookie session) ====================

#define SESSION_COOKIE  "wd_sid"
#define MAX_USER_LEN    32
#define MAX_PASS_LEN    64

static char s_session_token[33] = {0};   // 32 位 hex + NUL

static esp_err_t get_cookie(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    size_t len = out_len;
    esp_err_t ret = httpd_req_get_cookie_val(req, name, out, &len);
    if (ret == ESP_OK) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

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

static bool check_auth(httpd_req_t *req)
{
    char cookie_val[64] = {0};
    if (get_cookie(req, SESSION_COOKIE, cookie_val, sizeof(cookie_val)) == ESP_OK) {
        if (s_session_token[0] != '\0' && strcmp(cookie_val, s_session_token) == 0) {
            return true;
        }
    }
    return false;
}

static bool require_auth(httpd_req_t *req, bool is_api)
{
    if (check_auth(req)) return true;

    if (is_api) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"未登录\"}", -1);
    } else {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        httpd_resp_send(req, NULL, 0);
    }
    return false;
}

static void set_login_cookie(httpd_req_t *req)
{
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        sprintf(&s_session_token[i * 2], "%02x", rnd[i]);
    }

    static _Thread_local char cookie[128];   // 每连接独立, 避免并发登录互相覆盖
    snprintf(cookie, sizeof(cookie),
             SESSION_COOKIE "=%s; Path=/; HttpOnly; Max-Age=86400; SameSite=Lax", s_session_token);
    if (httpd_resp_set_hdr(req, "Set-Cookie", cookie) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set session cookie");
    }
}

static void clear_login_cookie(httpd_req_t *req)
{
    s_session_token[0] = '\0';
    httpd_resp_set_hdr(req, "Set-Cookie",
        SESSION_COOKIE "=; Path=/; HttpOnly; Max-Age=0");
}

// ==================== 前向声明 ====================

static esp_err_t handler_login_page(httpd_req_t *req);
static esp_err_t handler_login_post(httpd_req_t *req);
static esp_err_t handler_logout(httpd_req_t *req);
static esp_err_t handler_status(httpd_req_t *req);
static esp_err_t handler_dashboard(httpd_req_t *req);
static esp_err_t handler_reboot(httpd_req_t *req);
static esp_err_t handler_device_reboot(httpd_req_t *req);
static esp_err_t handler_poweron(httpd_req_t *req);
static esp_err_t handler_shutdown(httpd_req_t *req);
static esp_err_t handler_poweroff(httpd_req_t *req);
static esp_err_t handler_power_state(httpd_req_t *req);
static esp_err_t handler_logs(httpd_req_t *req);
static esp_err_t handler_settings(httpd_req_t *req);
static esp_err_t handler_change_password(httpd_req_t *req);
static esp_err_t handler_firmware(httpd_req_t *req);
static esp_err_t handler_ota_status(httpd_req_t *req);
static esp_err_t handler_reset_wifi(httpd_req_t *req);
static esp_err_t handler_favicon(httpd_req_t *req);

// ==================== 登录 / 登出 ====================

static esp_err_t handler_login_page(httpd_req_t *req)
{
    if (check_auth(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    const char *html =
        "<!DOCTYPE html>"
        "<html lang=\"zh-CN\">"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>登录 · 看门狗控制台</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:-apple-system,\"Microsoft YaHei\",sans-serif;"
        "background:radial-gradient(circle at 50% 20%,#1e1b4b 0,transparent 45%),#0f172a;"
        "color:#e2e8f0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
        ".card{width:100%;max-width:400px;background:rgba(30,41,59,.9);backdrop-filter:blur(12px);"
        "border:1px solid rgba(255,255,255,.08);border-radius:22px;padding:38px;box-shadow:0 24px 60px rgba(0,0,0,.45)}"
        ".logo{width:56px;height:56px;margin:0 auto 22px;border-radius:16px;background:linear-gradient(135deg,#38bdf8,#6366f1);"
        "display:flex;align-items:center;justify-content:center;font-size:28px;font-weight:800;color:#0f172a;"
        "box-shadow:0 8px 24px rgba(99,102,241,.4)}"
        "h1{text-align:center;color:#f1f5f9;font-size:21px;margin-bottom:6px}"
        ".sub{text-align:center;color:#64748b;font-size:13px;margin-bottom:28px}"
        "label{display:block;font-size:13px;color:#94a3b8;margin:14px 0 6px}"
        "input{width:100%;padding:13px 14px;border:1px solid #334155;border-radius:11px;background:#0f172a;color:#e2e8f0;font-size:15px}"
        "input:focus{outline:none;border-color:#38bdf8}"
        "button{width:100%;padding:14px;margin-top:24px;border:none;border-radius:11px;"
        "background:linear-gradient(135deg,#38bdf8,#0ea5e9);color:#0f172a;font-size:16px;font-weight:700;cursor:pointer;transition:opacity .2s}"
        "button:hover{opacity:.9}"
        ".err{margin-top:16px;padding:12px;border-radius:10px;background:#ef4444;color:#fff;text-align:center;font-size:13px;display:none}"
        ".tip{margin-top:22px;font-size:12px;color:#64748b;text-align:center;line-height:1.6}"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"card\">"
        "<div class=\"logo\">W</div>"
        "<h1>看门狗控制台</h1>"
        "<p class=\"sub\">Watchdog · 请登录</p>"
        "<form id=\"f\" method=\"POST\" action=\"/login\">"
        "<label>用户名</label>"
        "<input name=\"username\" type=\"text\" placeholder=\"用户名\" required maxlength=\"31\" autocomplete=\"username\">"
        "<label>密码</label>"
        "<input name=\"password\" type=\"password\" placeholder=\"密码\" required maxlength=\"63\" autocomplete=\"current-password\">"
        "<button type=\"submit\">登 录</button>"
        "</form>"
        "<div id=\"err\" class=\"err\"></div>"
        "<div class=\"tip\">默认账号: <b>admin</b> / <b>admin123</b><br>重置网络后恢复默认</div>"
        "</div>"
        "<script>"
        "var f=document.getElementById('f'),err=document.getElementById('err');"
        "f.addEventListener('submit',function(e){"
        "e.preventDefault();"
        "var d='username='+encodeURIComponent(f.username.value)+'&password='+encodeURIComponent(f.password.value);"
        "fetch('/login',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:d})"
        ".then(function(r){return r.json();})"
        ".then(function(d){"
        "if(d.status==='ok'){location.href='/';}"
        "else{err.style.display='block';err.textContent=d.message||'登录失败';}"
        "})"
        ".catch(function(e){err.style.display='block';err.textContent='请求失败: '+e.message;});"
        "});"
        "</script>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

static esp_err_t handler_login_post(httpd_req_t *req)
{
    char buf[256];
    int len = req->content_len;
    if (len > (int)sizeof(buf) - 1) {
        // 超长请求直接拒绝, 不静默截断 (截断会导致密码比对莫名失败)
        httpd_resp_set_status(req, "413 Content Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"请求过长\"}", -1);
        return ESP_OK;
    }
    if (len > 0) {
        // 循环读取: TCP 分段时单次 recv 可能只返回部分数据
        int r = 0;
        while (r < len) {
            int n = httpd_req_recv(req, buf + r, len - r);
            if (n <= 0) break;
            r += n;
        }
        if (r > 0) {
            buf[r] = '\0';

            char user_in[MAX_USER_LEN] = {0};
            char pass_in[MAX_PASS_LEN] = {0};
            httpd_query_key_value(buf, "username", user_in, sizeof(user_in));
            httpd_query_key_value(buf, "password", pass_in, sizeof(pass_in));
            url_decode(user_in);
            url_decode(pass_in);

            char real_user[MAX_USER_LEN] = {0};
            char real_pass[MAX_PASS_LEN] = {0};
            if (nvs_get_credentials(real_user, sizeof(real_user), real_pass, sizeof(real_pass)) != ESP_OK) {
                strcpy(real_user, DEFAULT_WEB_USERNAME);
                strcpy(real_pass, DEFAULT_WEB_PASSWORD);
            }

            if (strcmp(user_in, real_user) == 0 && strcmp(pass_in, real_pass) == 0) {
                set_login_cookie(req);
                LOG_I("Web 登录成功: %s", user_in);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"登录成功\"}", -1);
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "Login failed: user='%s'", user_in);
                LOG_W("Web 登录失败: %s", user_in);
            }
        }
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"用户名或密码错误\"}", -1);
    return ESP_OK;
}

static esp_err_t handler_logout(httpd_req_t *req)
{
    clear_login_cookie(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ==================== API Handlers ====================

static esp_err_t handler_status(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    watchdog_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    watchdog_get_stats(&stats);

    const char *state_str;
    switch (stats.state) {
        case WD_STATE_HEALTHY:       state_str = "healthy"; break;
        case WD_STATE_WARNING:       state_str = "warning"; break;
        case WD_STATE_SERVER_DOWN:   state_str = "down"; break;
        case WD_STATE_IDLE:          state_str = "idle"; break;
        default:                     state_str = "unknown"; break;
    }

    ota_status_t ota;
    ota_get_status(&ota);

    // IP / WiFi 信号强度 (控制台状态卡显示; 获取失败时显示占位符)
    char ip_str[16] = "-";
    int8_t rssi = 0;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    char json[960];
    snprintf(json, sizeof(json),
        "{"
        "\"state\":%d,"
        "\"state_str\":\"%s\","
        "\"usb_connected\":%s,"
        "\"heartbeat_count\":%lu,"
        "\"response_count\":%lu,"
        "\"timeout_count\":%lu,"
        "\"consecutive_timeouts\":%lu,"
        "\"consecutive_reboots\":%lu,"
        "\"watchdog_running\":%s,"
        "\"paused\":%s,"
        "\"poweroff_enabled\":%s,"
        "\"notify_enabled\":%s,"
        "\"uptime_s\":%lu,"
        "\"reboots\":%lu,"
        "\"wallclock_s\":%lu,"
        "\"time_synced\":%s,"
        "\"ip\":\"%s\","
        "\"rssi\":%d,"
        "\"ota\":{\"state\":%d,\"progress\":%u,\"received\":%lu,\"total\":%lu}"
        "}",
        (int)stats.state,
        state_str,
        usb_is_connected() ? "true" : "false",
        (unsigned long)stats.heartbeat_count,
        (unsigned long)stats.response_count,
        (unsigned long)stats.timeout_count,
        (unsigned long)stats.consecutive_timeouts,
        (unsigned long)stats.consecutive_reboots,
        stats.running ? "true" : "false",
        stats.paused ? "true" : "false",
        stats.auto_poweroff_enabled ? "true" : "false",
        notify_is_enabled() ? "true" : "false",
        (unsigned long)uptime_get_seconds(),
        (unsigned long)uptime_get_reboots(),
        (unsigned long)(event_log_time_synced() ? time(NULL) : 0UL),
        event_log_time_synced() ? "true" : "false",
        ip_str,
        (int)rssi,
        (int)ota.state,
        (unsigned int)ota.progress,
        (unsigned long)ota.received_bytes,
        (unsigned long)ota.total_bytes
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static void json_escape(const char *in, char *out, size_t out_size)
{
    if (out_size == 0) return;
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 8 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
            case '"':  memcpy(out + o, "\\\"", 2); o += 2; break;
            case '\\': memcpy(out + o, "\\\\", 2); o += 2; break;
            case '\n': memcpy(out + o, "\\n", 2);  o += 2; break;
            case '\r': memcpy(out + o, "\\r", 2);  o += 2; break;
            case '\t': memcpy(out + o, "\\t", 2);  o += 2; break;
            case '<':  memcpy(out + o, "\\u003c", 6); o += 6; break;
            case '>':  memcpy(out + o, "\\u003e", 6); o += 6; break;
            case '&':  memcpy(out + o, "\\u0026", 6); o += 6; break;
            default:
                if (c < 0x20) {
                    o += (size_t)snprintf(out + o, out_size - o, "\\u%04x", c);
                } else {
                    out[o++] = (char)c;
                }
        }
    }
    out[o] = '\0';
}

// HTML 属性值转义 (用于把用户配置的通知地址回填到 input value)
static void html_escape_attr(const char *in, char *out, size_t out_size)
{
    if (out_size == 0) return;
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 8 < out_size; i++) {
        switch (in[i]) {
            case '&':  memcpy(out + o, "&amp;", 5);  o += 5; break;
            case '"':  memcpy(out + o, "&quot;", 6); o += 6; break;
            case '\'': memcpy(out + o, "&#39;", 5);  o += 5; break;
            case '<':  memcpy(out + o, "&lt;", 4);   o += 4; break;
            case '>':  memcpy(out + o, "&gt;", 4);   o += 4; break;
            default:   out[o++] = in[i]; break;
        }
    }
    out[o] = '\0';
}

static esp_err_t handler_logs(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    const uint32_t ram_max = 32, nvs_max = 32;
    log_entry_t *ram = calloc(ram_max, sizeof(log_entry_t));
    log_entry_t *nvs = calloc(nvs_max, sizeof(log_entry_t));
    if (!ram || !nvs) {
        free(ram); free(nvs);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    uint32_t ram_cnt = event_log_read(ram, ram_max);
    uint32_t nvs_cnt = event_log_read_nvs(nvs, nvs_max);

    // NVS 中 WARN+ 条目与本次 RAM 里的同 seq 条目重复, 去重
    bool *nvs_skip = calloc(nvs_cnt, sizeof(bool));
    if (nvs_skip) {
        for (uint32_t i = 0; i < nvs_cnt; i++) {
            for (uint32_t j = 0; j < ram_cnt; j++) {
                if (nvs[i].seq == ram[j].seq) { nvs_skip[i] = true; break; }
            }
        }
    }

    const size_t cap = 16384;
    char *json = malloc(cap);
    if (!json) {
        free(ram); free(nvs);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

#define JSON_LEFT()  ((size_t)(p - json) >= cap ? (size_t)0 : (size_t)(cap - (size_t)(p - json)))

    char *p = json;
    p += snprintf(p, JSON_LEFT(), "{\"ram_count\":%lu,\"nvs_count\":%lu,\"entries\":[",
                  (unsigned long)ram_cnt, (unsigned long)nvs_cnt);

    bool first = true;
    bool truncated = false;
    const log_entry_t *lists[2] = { nvs, ram };
    const uint32_t counts[2]    = { nvs_cnt, ram_cnt };

    char esc[LOG_MAX_MESSAGE_LEN * 6 + 8];

    for (int l = 0; l < 2 && !truncated; l++) {
        for (uint32_t i = 0; i < counts[l]; i++) {
            if (l == 0 && nvs_skip && nvs_skip[i]) continue;
            char tbuf[32];
            event_log_format_time(lists[l][i].timestamp_ms, lists[l][i].wallclock_s,
                                  tbuf, sizeof(tbuf));
            json_escape(lists[l][i].message, esc, sizeof(esc));
            if (JSON_LEFT() < 320) { truncated = true; break; }
            int n = snprintf(p, JSON_LEFT(),
                "%s{\"time\":\"%s\",\"level\":%d,\"message\":\"%s\"}",
                first ? "" : ",", tbuf, (int)lists[l][i].level, esc);
            if (n < 0 || (size_t)n >= JSON_LEFT()) { truncated = true; break; }
            p += n;
            first = false;
        }
    }
#undef JSON_LEFT

    if ((size_t)(p - json) + 3 < cap) {
        p += snprintf(p, 3, "]}");
    } else {
        strcpy(json + cap - 3, "]}");
    }
    (void)truncated;

    free(nvs_skip);
    free(ram);
    free(nvs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

static void reboot_task(void *pv)
{
    ESP_LOGI(TAG, "Web 触发服务器重启");
    LOG_W("Web 触发服务器 GPIO 复位");
    gpio_trigger_server_reset(500);
    vTaskDelete(NULL);
}



static esp_err_t handler_reboot(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;
    xTaskCreate(reboot_task, "reboot_task", 2048, NULL, 5, NULL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"已发送重启脉冲\"}", -1);
    return ESP_OK;
}

// 软关机: 短按电源键让服务器自行关机, 之后暂停看门狗监控
// (否则服务器停止回复心跳会被误判宕机, 又被 GPIO 复位按开机)
static void shutdown_task(void *pv)
{
    ESP_LOGI(TAG, "Web 触发服务器软关机 (短按电源键)");
    LOG_W("Web 触发服务器软关机, 随后暂停看门狗监控");
    gpio_trigger_server_poweron(CONFIG_POWERON_PULSE_MS);

    // 给系统留出关机时间 (系统正常关机通常需要若干秒), 再暂停监控
    vTaskDelay(pdMS_TO_TICKS(3000));
    watchdog_pause();
    vTaskDelete(NULL);
}

static esp_err_t handler_shutdown(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;
    xTaskCreate(shutdown_task, "shutdown_task", 2048, NULL, 5, NULL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"已发送关机脉冲，监控将在 3 秒后暂停\"}", -1);
    return ESP_OK;
}

// 重启看门狗设备本身 (esp_restart), 与 /api/reboot (GPIO 复位"服务器") 语义不同
static void device_reboot_task(void *pv)
{
    ESP_LOGI(TAG, "Web 触发看门狗设备重启");
    LOG_W("Web 触发看门狗设备重启");
    vTaskDelay(pdMS_TO_TICKS(500));   // 让 HTTP 响应先发出
    esp_restart();
    vTaskDelete(NULL);
}

static esp_err_t handler_device_reboot(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;
    xTaskCreate(device_reboot_task, "dev_reboot", 2048, NULL, 5, NULL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"设备重启中\"}", -1);
    return ESP_OK;
}

static void poweron_task(void *pv)
{
    ESP_LOGI(TAG, "Web 触发服务器开机");
    LOG_I("Web 触发服务器开机");
    gpio_trigger_server_poweron(0);

    // Web 端执行开机控制 = 管理员已修复故障:
    // 清零连续重启计数; 若看门狗因"强制关机/重启过多"而停止, 一并恢复监控
    watchdog_resume();
    vTaskDelete(NULL);
}

static esp_err_t handler_poweron(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;
    xTaskCreate(poweron_task, "poweron_task", 2048, NULL, 5, NULL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"已发送开机脉冲\"}", -1);
    return ESP_OK;
}

static void poweroff_task(void *pv)
{
    ESP_LOGW(TAG, "Web 触发强制关机 (长按 5 秒)");
    LOG_W("Web 触发强制关机 (长按 5 秒)");
    gpio_force_poweroff();
    vTaskDelete(NULL);
}

static esp_err_t handler_poweroff(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;
    xTaskCreate(poweroff_task, "poweroff_task", 2048, NULL, 5, NULL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"已发送强制关机信号(长按5秒)\"}", -1);
    return ESP_OK;
}

static esp_err_t handler_power_state(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    power_state_t st = gpio_get_power_state();
    const char *str = (st == POWER_STATE_ON) ? "on"
                    : (st == POWER_STATE_OFF) ? "off" : "unknown";

    char json[256];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"gpio\":%d,\"wired\":%s}",
        str, CONFIG_POWER_DETECT_GPIO_PIN,
        gpio_power_detect_available() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static esp_err_t handler_settings(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    // 查询串里可能同时带心跳参数、自动关机开关和通知地址(较长), 缓冲要够
    char query[768];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"缺少查询参数\"}", -1);
        return ESP_OK;
    }

    char interval_str[16] = {0};
    char timeout_str[16]  = {0};
    char poweroff_str[8]  = {0};
    char notify_str[8]    = {0};
    char url_str[NOTIFY_URL_MAX_LEN * 3] = {0};

    bool has_interval = (httpd_query_key_value(query, "interval", interval_str, sizeof(interval_str)) == ESP_OK);
    bool has_timeout  = (httpd_query_key_value(query, "timeout",  timeout_str,  sizeof(timeout_str))  == ESP_OK);
    bool has_poweroff = (httpd_query_key_value(query, "poweroff", poweroff_str, sizeof(poweroff_str)) == ESP_OK);
    bool has_notify   = (httpd_query_key_value(query, "notify",   notify_str,   sizeof(notify_str))   == ESP_OK);
    bool has_url      = (httpd_query_key_value(query, "notify_url", url_str, sizeof(url_str)) == ESP_OK);

    char msg[192] = "参数已保存";

    if (has_interval && has_timeout) {
        uint32_t interval = (uint32_t)atoi(interval_str);
        uint32_t timeout = (uint32_t)atoi(timeout_str);

        /* 最大值 / 最小值约束, 防止无效参数 */
        if (interval < 1) interval = 1;
        if (interval > 3600) interval = 3600;   // 上限 1 小时, 避免 uint32 溢出与无意义周期
        if (timeout < 60) timeout = 60;         // 至少 1 分钟
        if (timeout > 86400) timeout = 86400;  // 上限 24 小时
        if (timeout < interval * 2) timeout = interval * 2;  // 超时至少是间隔的 2 倍

        watchdog_set_params(interval, timeout);
        nvs_save_heartbeat_params(interval, timeout);

        ESP_LOGI(TAG, "参数已更新: interval=%lu, timeout=%lu",
                 (unsigned long)interval, (unsigned long)timeout);
        LOG_I("心跳参数已更新: %lu 秒 / %lu 秒", (unsigned long)interval, (unsigned long)timeout);
    }

    if (has_poweroff) {
        bool enabled = (atoi(poweroff_str) != 0);
        watchdog_set_auto_poweroff(enabled);
        nvs_save_auto_poweroff(enabled);
        LOG_I("自动强制关机已%s", enabled ? "启用" : "禁用");
    }

    if (has_notify || has_url) {
        bool enabled = has_notify ? (atoi(notify_str) != 0) : notify_is_enabled();
        // 请求里没带 notify_url 时保持原地址, 避免因缺少参数把配置清空
        const char *final_url = notify_get_url();

        if (has_url) {
            url_decode(url_str);

            // 只允许 http(s):// 开头, 防止误填
            if (strlen(url_str) > 0 &&
                strncmp(url_str, "http://", 7) != 0 && strncmp(url_str, "https://", 8) != 0) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"推送地址需以 http:// 或 https:// 开头\"}", -1);
                return ESP_OK;
            }
            final_url = url_str;
        }

        // 启用了通知但地址为空 -> 视为无效, 直接拒绝, 避免"开了但发不出去"
        if (enabled && strlen(final_url) == 0) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"已启用通知但推送地址为空\"}", -1);
            return ESP_OK;
        }

        notify_set_config(enabled, final_url);
        LOG_I("通知推送已%s", enabled ? "启用" : "禁用");
        snprintf(msg, sizeof(msg), "通知设置已保存 (%s)", enabled ? "启用" : "禁用");
    }

    httpd_resp_set_type(req, "application/json");
    char body[256];
    snprintf(body, sizeof(body), "{\"status\":\"ok\",\"message\":\"%s\"}", msg);
    httpd_resp_send(req, body, strlen(body));
    return ESP_OK;
}

static esp_err_t handler_notify_test(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    if (!notify_is_enabled() || strlen(notify_get_url()) == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"请先启用通知并填写推送地址\"}", -1);
        return ESP_OK;
    }

    char body[160];
    snprintf(body, sizeof(body), "这是一条来自 ESP32-C3 看门狗的测试通知");
    notify_send_async("看门狗测试", body);
    LOG_I("已发送测试通知");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"测试通知已发送, 请查看接收端\"}", -1);
    return ESP_OK;
}

static esp_err_t handler_reset_stats(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    watchdog_reset_stats();
    uptime_reset_reboots();

    ESP_LOGI(TAG, "Web 清零心跳统计");
    LOG_I("Web 清零心跳统计");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"统计已清零\"}", -1);
    return ESP_OK;
}

static esp_err_t handler_change_password(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    char query[512];   // 三个字段 URL 编码后可能超过 256, 过小会导致改密"假成功"
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char new_user[MAX_USER_LEN] = {0};
        char new_pass[MAX_PASS_LEN] = {0};
        char old_pass[MAX_PASS_LEN] = {0};
        httpd_query_key_value(query, "username", new_user, sizeof(new_user));
        httpd_query_key_value(query, "password", new_pass, sizeof(new_pass));
        httpd_query_key_value(query, "old_password", old_pass, sizeof(old_pass));
        url_decode(new_user);
        url_decode(new_pass);
        url_decode(old_pass);

        char real_pass[MAX_PASS_LEN] = {0};
        char real_user[MAX_USER_LEN] = {0};
        nvs_get_credentials(real_user, sizeof(real_user), real_pass, sizeof(real_pass));

        if (strlen(old_pass) > 0 && strcmp(old_pass, real_pass) != 0) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"当前密码错误\"}", -1);
            return ESP_OK;
        }

        if (strlen(new_pass) < 4 && strlen(new_pass) > 0) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"新密码至少 4 位\"}", -1);
            return ESP_OK;
        }

        if (strlen(new_user) == 0) {
            strcpy(new_user, real_user);
        }

        if (strlen(new_pass) == 0) {
            strcpy(new_pass, real_pass);
        }

        esp_err_t ret = nvs_save_credentials(new_user, new_pass);
        if (ret != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_OK;
        }

        LOG_I("登录凭据已更新 (操作者: %s)", real_user);
        clear_login_cookie(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"凭据已更新，请重新登录\"}", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"请求参数过长或格式错误\"}", -1);
    return ESP_OK;
}

// ==================== 固件 OTA 上传 ====================

static void firmware_reboot_task(void *pv)
{
    // 延迟重启, 确保 "刷写成功" 的 HTTP 响应已发回前端
    vTaskDelay(pdMS_TO_TICKS(2500));
    ESP_LOGI(TAG, "OTA rebooting...");
    LOG_W("OTA 正在重启设备");
    esp_restart();
    vTaskDelete(NULL);
}

static esp_err_t handler_firmware(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    if (req->method != HTTP_POST) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // 禁止并发升级 (OTA 状态机非重入)
    if (ota_get_state() == OTA_STATE_WRITING) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"已有升级正在进行中\",\"code\":409}", -1);
        return ESP_OK;
    }

    // Content-Length 必须, 用于容量预校验 (未知大小时无法判断是否超过 OTA 分区)
    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "411 Length Required");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"请使用 Content-Length 头指定固件大小\",\"code\":411}", -1);
        return ESP_OK;
    }

    // 分区容量预检查
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"未找到可用 OTA 分区\",\"code\":500}", -1);
        return ESP_OK;
    }
    if ((uint32_t)req->content_len > next->size) {
        char body[256];
        snprintf(body, sizeof(body),
            "{\"status\":\"error\",\"message\":\"固件过大 (%ld KB > 分区 %lu KB)\",\"code\":413}",
            (long)(req->content_len / 1024), (unsigned long)(next->size / 1024));
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, body, strlen(body));
        return ESP_OK;
    }

    // 校验 Content-Type (推荐 application/octet-stream; 兼容 multipart)
    char content_type[64] = {0};
    size_t ct_len = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (ct_len > 0 && ct_len < sizeof(content_type)) {
        httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    }
    if (strlen(content_type) > 0 &&
        strstr(content_type, "application/octet-stream") == NULL &&
        strstr(content_type, "multipart/form-data") == NULL) {
        ESP_LOGW(TAG, "Unusual Content-Type: %s", content_type);
    }

    ESP_LOGI(TAG, "Firmware upload: %ld bytes, partition=%s (%lu KB free)",
             (long)req->content_len, next->label, (unsigned long)(next->size / 1024));
    LOG_I("开始上传固件: %ld 字节", (long)req->content_len);

    esp_err_t ret = ota_handle_raw_upload(req, (uint32_t)req->content_len);

    if (ret == ESP_OK) {
        LOG_I("固件刷写成功, 准备重启设备");
        xTaskCreate(firmware_reboot_task, "fw_reboot", 2048, NULL, 5, NULL);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"固件刷写成功，设备即将重启...\"}", -1);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Firmware upload failed: %s", esp_err_to_name(ret));
        LOG_E("Firmware upload failed: %s", esp_err_to_name(ret));
        int code = 400;
        const char *msg = "固件刷写失败";
        if (ret == ESP_ERR_OTA_VALIDATE_FAILED) { msg = "非法固件文件 (镜像头校验失败)"; code = 415; }
        else if (ret == ESP_ERR_INVALID_SIZE) { msg = "固件大小不合法 (过大或非预期)"; code = 413; }
        else if (ret == ESP_ERR_NO_MEM) { msg = "内存不足, 请重试"; code = 503; }
        else if (ret == ESP_ERR_INVALID_STATE) { msg = "升级状态异常, 请稍后重试"; code = 409; }
        httpd_resp_set_status(req, code == 400 ? "400 Bad Request" : (code == 415 ? "415 Unsupported Media Type" : (code == 413 ? "413 Payload Too Large" : "503 Service Unavailable")));
        httpd_resp_set_type(req, "application/json");
        char body[256];
        snprintf(body, sizeof(body), "{\"status\":\"error\",\"message\":\"%s\",\"code\":%d}", msg, code);
        httpd_resp_send(req, body, strlen(body));
        return ESP_OK;
    }
}

static esp_err_t handler_ota_status(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    ota_status_t st;
    ota_get_status(&st);

    const char *state_str;
    switch (st.state) {
        case OTA_STATE_WRITING:  state_str = "writing"; break;
        case OTA_STATE_SUCCESS:  state_str = "success"; break;
        case OTA_STATE_FAILED:   state_str = "failed"; break;
        default:                 state_str = "idle"; break;
    }

    // 附加当前/下一块分区信息, 便于前端诊断
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    char json[512];
    snprintf(json, sizeof(json),
        "{"
        "\"state\":\"%s\",\"state_code\":%d,"
        "\"progress\":%u,\"received\":%lu,\"total\":%lu,"
        "\"running_partition\":\"%s\","
        "\"next_partition\":\"%s\","
        "\"reboot_pending\":%s"
        "}",
        state_str, (int)st.state,
        (unsigned int)st.progress,
        (unsigned long)st.received_bytes,
        (unsigned long)st.total_bytes,
        running ? running->label : "-",
        next ? next->label : "-",
        ota_is_reboot_pending() ? "true" : "false"
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static esp_err_t handler_reset_wifi(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    ESP_LOGI(TAG, "Web 请求恢复出厂设置 (WiFi + 心跳参数 + 自动保护 + 通知 + 凭据)");
    LOG_W("Web 触发恢复出厂设置 (所有自定义参数已恢复默认)");

    // 恢复默认时同时清空事件日志, 避免旧日志继续堆积
    event_log_clear_ram();
    event_log_clear_nvs();

    nvs_factory_reset();   // 清空 WiFi / 心跳参数 / 自动保护 / 通知 / 凭据

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"已恢复出厂设置, 正在重启...\"}", -1);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_clear_logs(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;
    event_log_clear_ram();
    event_log_clear_nvs();
    LOG_I("Web 清除事件日志");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"日志已清空\"}", -1);
    return ESP_OK;
}

static esp_err_t handler_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ==================== Dashboard (美化版) ====================

static void ph_replace(char *buf, size_t buf_size, const char *key, const char *val)
{
    size_t klen = strlen(key), vlen = strlen(val);
    char *pos = buf;
    while ((pos = strstr(pos, key)) != NULL) {
        size_t used = (size_t)(pos - buf);
        size_t tail = strlen(pos + klen);
        if (used + vlen + tail + 1 > buf_size) return;
        memmove(pos + vlen, pos + klen, tail + 1);
        memcpy(pos, val, vlen);
        pos += vlen;
    }
}

static esp_err_t handler_dashboard(httpd_req_t *req)
{
    if (!require_auth(req, false)) return ESP_OK;

    watchdog_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    watchdog_get_stats(&stats);

    uint32_t hb_interval = 60, hb_timeout = 600;
    watchdog_get_params(&hb_interval, &hb_timeout);

    const char *state_text;
    const char *led_color;
    bool pulse = false;
    switch (stats.state) {
        case WD_STATE_HEALTHY:
            state_text = "系统正常 · 心跳正常";
            led_color = "#22c55e";
            pulse = true;
            break;
        case WD_STATE_WARNING:
            state_text = "警告 · 即将超时";
            led_color = "#f59e0b";
            break;
        case WD_STATE_SERVER_DOWN:
            state_text = "服务器宕机 · 正在重启";
            led_color = "#ef4444";
            break;
        default:
            state_text = "USB 已断开";
            led_color = "#64748b";
            break;
    }

    // 固件版本 + 当前运行分区
    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *part_name = running ? running->label : "-";

    // 注意: esp_ota_get_app_description() 自 IDF v5.0 起已废弃并移除,
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *version = (app_desc && strlen(app_desc->version) > 0) ? app_desc->version : "unknown";
    const char *build_date = (app_desc && strlen(app_desc->date) > 0) ? app_desc->date : "unknown";

    const char *tpl = dashboard_get_html();

    // 通知地址需要做 HTML 属性转义 (可能含 & " 等字符)
    char notify_url_esc[NOTIFY_URL_MAX_LEN * 6 + 8];
    html_escape_attr(notify_get_url(), notify_url_esc, sizeof(notify_url_esc));

    // 通知地址回填最长可达 6 倍 (全被转义), 预留足量空间, 避免占位符替换失败
    size_t cap = strlen(tpl) + strlen(notify_url_esc) + 1024;
    char *html = malloc(cap);
    if (!html) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    snprintf(html, cap, "%s", tpl);

    char v_interval[16], v_timeout[16];
    char v_hb[16], v_resp[16], v_to[16], v_reboot[16], v_consec[16];
    snprintf(v_interval, sizeof(v_interval), "%lu", (unsigned long)hb_interval);
    snprintf(v_timeout,  sizeof(v_timeout),  "%lu", (unsigned long)hb_timeout);
    snprintf(v_hb,       sizeof(v_hb),       "%lu", (unsigned long)stats.heartbeat_count);
    snprintf(v_resp,     sizeof(v_resp),     "%lu", (unsigned long)stats.response_count);
    snprintf(v_to,       sizeof(v_to),       "%lu", (unsigned long)stats.timeout_count);
    snprintf(v_reboot,   sizeof(v_reboot),   "%lu", (unsigned long)uptime_get_reboots());
    snprintf(v_consec,   sizeof(v_consec),   "%lu", (unsigned long)stats.consecutive_reboots);

    const struct { const char *key; const char *val; } ph[] = {
        { "{{VERSION}}",       version },
        { "{{BUILD_DATE}}",    build_date },
        { "{{LED_CLASS}}",     pulse ? "pulse" : "" },
        { "{{LED_COLOR}}",     led_color },
        { "{{STATE_TEXT}}",    state_text },
        { "{{USB_STATE}}",     usb_is_connected() ? "Connected" : "Disconnected" },
        { "{{INTERVAL}}",      v_interval },
        { "{{TIMEOUT}}",       v_timeout },
        { "{{PARTITION}}",     part_name },
        { "{{HB_COUNT}}",      v_hb },
        { "{{RESP_COUNT}}",    v_resp },
        { "{{TIMEOUT_COUNT}}", v_to },
        { "{{REBOOT_COUNT}}",  v_reboot },
        { "{{CONSEC_REBOOTS}}", v_consec },
        { "{{POWEROFF_STATE}}", watchdog_get_auto_poweroff() ? "checked" : "" },
        { "{{NOTIFY_STATE}}",   notify_is_enabled() ? "checked" : "" },
        { "{{NOTIFY_URL}}",     notify_url_esc },
    };
    for (size_t i = 0; i < sizeof(ph) / sizeof(ph[0]); i++) {
        ph_replace(html, cap, ph[i].key, ph[i].val);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, strlen(html));
    free(html);
    return ESP_OK;
}

// ==================== Public API ====================

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 12288;
    config.max_uri_handlers = 32;
    config.max_resp_headers = 16;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t routes[] = {
        // 认证 (无需登录)
        { .uri = "/login",            .method = HTTP_GET,  .handler = handler_login_page },
        { .uri = "/login",            .method = HTTP_POST, .handler = handler_login_post },
        { .uri = "/logout",           .method = HTTP_GET,  .handler = handler_logout },
        // 页面
        { .uri = "/",                 .method = HTTP_GET,  .handler = handler_dashboard },
        // API
        { .uri = "/api/status",           .method = HTTP_GET,  .handler = handler_status },
        { .uri = "/api/logs",             .method = HTTP_GET,  .handler = handler_logs },
        { .uri = "/api/reboot",           .method = HTTP_POST, .handler = handler_reboot },
        { .uri = "/api/device_reboot",    .method = HTTP_POST, .handler = handler_device_reboot },
        { .uri = "/api/poweron",          .method = HTTP_POST, .handler = handler_poweron },
        { .uri = "/api/shutdown",         .method = HTTP_POST, .handler = handler_shutdown },
        { .uri = "/api/poweroff",         .method = HTTP_POST, .handler = handler_poweroff },
        { .uri = "/api/power-state",      .method = HTTP_GET,  .handler = handler_power_state },
        { .uri = "/api/settings",         .method = HTTP_POST, .handler = handler_settings },
        { .uri = "/api/change_password",  .method = HTTP_POST, .handler = handler_change_password },
        { .uri = "/api/firmware",         .method = HTTP_POST, .handler = handler_firmware },
        { .uri = "/api/ota/status",       .method = HTTP_GET,  .handler = handler_ota_status },
        { .uri = "/api/reset_wifi",       .method = HTTP_POST, .handler = handler_reset_wifi },
        { .uri = "/api/clear_logs",       .method = HTTP_POST, .handler = handler_clear_logs },
        { .uri = "/api/reset_stats",      .method = HTTP_POST, .handler = handler_reset_stats },
        { .uri = "/api/notify_test",      .method = HTTP_POST, .handler = handler_notify_test },
        { .uri = "/favicon.ico",          .method = HTTP_GET,  .handler = handler_favicon },
    };

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t r = httpd_register_uri_handler(s_server, &routes[i]);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register route: %s", routes[i].uri);
        }
    }

    ESP_LOGI(TAG, "Web server started on port %d (auth + OTA enabled, default: %s / %s)",
             config.server_port, DEFAULT_WEB_USERNAME, DEFAULT_WEB_PASSWORD);
    return ESP_OK;
}
