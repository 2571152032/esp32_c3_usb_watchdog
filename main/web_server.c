/**
 * @file web_server.c
 * @brief Web 控制端实现 (ESP-IDF v6.1 兼容)
 *
 * 安全: 所有页面与 API 均通过 check_auth() 校验登录态 (cookie session),
 *       未登录 → 页面 302 跳转 /login, API 返回 401 JSON.
 *       默认用户名/密码 admin / admin123 (nvs_storage.h 宏定义),
 *       重置网络后恢复为默认值.
 *
 * 功能: 状态监控 / 参数设置 / 修改凭据 / 固件 OTA 更新 (拖拽上传) /
 *       服务器电源控制 / 实时事件日志 / 运行时间
 *
 * OTA 流程: 前端以 application/octet-stream 发送 .bin → 后端校验 (大小/镜像头/分区容量)
 *        → 流式写入 OTA 分区 → esp_ota_set_boot_partition → 延迟重启 → 前端轮询进度
 */

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

#include "web_server.h"
#include "watchdog.h"
#include "usb_device.h"
#include "gpio_control.h"
#include "nvs_storage.h"
#include "ota_update.h"
#include "event_log.h"
#include "uptime.h"
#include "dashboard_html.h"

static const char *TAG = "WEB";

static httpd_handle_t s_server = NULL;

// ==================== 会话管理 (简易 cookie session) ====================

#define SESSION_COOKIE  "wd_sid"
#define MAX_USER_LEN    32
#define MAX_PASS_LEN    64

/* 每次登录成功后随机生成的会话 token。
 * 之前是硬编码常量 "wd_ok", 知道固件源码的人可直接伪造 cookie 绕过登录。 */
static char s_session_token[33] = {0};   // 32 位 hex + NUL

static esp_err_t get_cookie(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hdr_len == 0) return ESP_FAIL;

    char *hdr = malloc(hdr_len + 1);
    if (!hdr) return ESP_FAIL;
    if (httpd_req_get_hdr_value_str(req, "Cookie", hdr, hdr_len + 1) != ESP_OK) {
        free(hdr);
        return ESP_FAIL;
    }

    char *save;
    char *token = strtok_r(hdr, ";", &save);
    while (token) {
        while (*token == ' ' || *token == '\t') token++;
        size_t nlen = strlen(name);
        if (strncmp(token, name, nlen) == 0 && token[nlen] == '=') {
            strncpy(out, token + nlen + 1, out_len - 1);
            out[out_len - 1] = '\0';
            free(hdr);
            return ESP_OK;
        }
        token = strtok_r(NULL, ";", &save);
    }
    free(hdr);
    return ESP_FAIL;
}

/* URL 解码 (application/x-www-form-urlencoded)。
 * 前端用 encodeURIComponent() 编码提交, 而 httpd_query_key_value 只做匹配不解码。
 * 若不解码, 密码里的 & + % 空格 中文 等会以 %26 %2B 形式存入/比对, 登录永远失败。 */
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
    // 每次登录生成随机 token; 重启后 s_session_token 清零, 旧 cookie 自动失效
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        sprintf(&s_session_token[i * 2], "%02x", rnd[i]);
    }

    char cookie[96];
    snprintf(cookie, sizeof(cookie),
             SESSION_COOKIE "=%s; Path=/; HttpOnly; Max-Age=86400", s_session_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

static void clear_login_cookie(httpd_req_t *req)
{
    s_session_token[0] = '\0';   // 服务端立即失效, 旧 cookie 不再可用
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
static esp_err_t handler_poweron(httpd_req_t *req);
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
        "<p class=\"sub\">ESP32-C3 USB Watchdog · 请登录</p>"
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
        "fetch('/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:d})"
        ".then(function(r){return r.json();})"
        ".then(function(d){"
        "if(d.status==='ok'){location.href='/';}"
        "else{err.style.display='block';err.textContent=d.message||'登录失败';}"
        "});"
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
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
    if (len > 0) {
        int r = httpd_req_recv(req, buf, len);
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
                LOG_I("Web login success: %s", user_in);
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"登录成功\"}", -1);
                return ESP_OK;
            } else {
                ESP_LOGW(TAG, "Login failed: user='%s'", user_in);
                LOG_W("Web login failed: %s", user_in);
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

    char json[768];
    snprintf(json, sizeof(json),
        "{"
        "\"state\":%d,"
        "\"state_str\":\"%s\","
        "\"usb_connected\":%s,"
        "\"heartbeat_count\":%lu,"
        "\"response_count\":%lu,"
        "\"timeout_count\":%lu,"
        "\"consecutive_timeouts\":%lu,"
        "\"uptime_s\":%lu,"
        "\"reboots\":%lu,"
        "\"ota\":{\"state\":%d,\"progress\":%u,\"received\":%lu,\"total\":%lu}"
        "}",
        (int)stats.state,
        state_str,
        usb_is_connected() ? "true" : "false",
        (unsigned long)stats.heartbeat_count,
        (unsigned long)stats.response_count,
        (unsigned long)stats.timeout_count,
        (unsigned long)stats.consecutive_timeouts,
        (unsigned long)uptime_get_seconds(),
        (unsigned long)uptime_get_reboots(),
        (int)ota.state,
        (unsigned int)ota.progress,
        (unsigned long)ota.received_bytes,
        (unsigned long)ota.total_bytes
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* JSON 字符串转义。
 * 必要性: 日志内容里会包含 WiFi SSID 等用户输入。若 SSID 含双引号或反斜杠,
 *         直接 %s 拼进去会生成非法 JSON, 前端 r.json() 抛异常, 日志面板
 *         直接空白。含 < > 时还会破坏 innerHTML 渲染 (注入)。
 * 输出缓冲区建议 >= 原文长度 * 2 + 1。
 */
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
            // 同时转义 HTML 敏感字符, 防止日志内容破坏页面 / 注入
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

static esp_err_t handler_logs(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    // 注意: log_entry_t 约 140 字节, 若放栈上会撑爆 httpd 任务栈
    //       (Stack protection fault 崩溃), 因此全部改用堆分配。
    //
    // 数量取舍: 64 条 → 32 条。
    //   ESP32-C3 空闲堆约 80-110KB, 而 dashboard 页面本身要 ~20KB (模板渲染)。
    //   前端每 4s 轮询 /api/logs, 若与 dashboard 请求并发, 旧版 33KB + 20KB
    //   会明显加剧堆碎片, 极端情况下分配失败返回 500。
    //   32+32=64 条足够展示 (前端只显示最近若干条), 内存降到约 25KB。
    const uint32_t ram_max = 32, nvs_max = 32;
    log_entry_t *ram = calloc(ram_max, sizeof(log_entry_t));
    log_entry_t *nvs = calloc(nvs_max, sizeof(log_entry_t));
    if (!ram || !nvs) {
        free(ram); free(nvs);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // 合并 RAM (最近) + NVS (持久) 日志, 前端按时间倒序展示
    uint32_t ram_cnt = event_log_read(ram, ram_max);
    uint32_t nvs_cnt = event_log_read_nvs(nvs, nvs_max);

    // 单条 JSON 最坏约 168 字节 (time+level+127 字符 msg), 64 条 → 约 10.8KB。
    // 给 16KB 留足余量。
    const size_t cap = 16384;
    char *json = malloc(cap);
    if (!json) {
        free(ram); free(nvs);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // 剩余空间计算: 必须防止下溢!
    // 若 p 越过 json+cap, "cap - (p - json)" 是无符号数会下溢成巨大值,
    // snprintf 就会疯狂越界写 —— 这是比栈溢出更隐蔽的崩溃源。
#define JSON_LEFT()  ((size_t)(p - json) >= cap ? (size_t)0 : (size_t)(cap - (size_t)(p - json)))

    char *p = json;
    p += snprintf(p, JSON_LEFT(), "{\"ram_count\":%lu,\"nvs_count\":%lu,\"entries\":[",
                  (unsigned long)ram_cnt, (unsigned long)nvs_cnt);

    // 先输出持久化日志 (旧), 再输出 RAM (新)
    bool first = true;
    bool truncated = false;
    const log_entry_t *lists[2] = { nvs, ram };
    const uint32_t counts[2]    = { nvs_cnt, ram_cnt };

    // message 最长 (LOG_MAX_MESSAGE_LEN-1)=127 字符; 最坏情况每个字符都转成
    // \uXXXX (6 字节) → 需要 127*6=762 字节。给 6 倍 + 8 保证绝不截断。
    char esc[LOG_MAX_MESSAGE_LEN * 6 + 8];

    for (int l = 0; l < 2 && !truncated; l++) {
        for (uint32_t i = 0; i < counts[l]; i++) {
            char tbuf[16];
            event_log_format_time(lists[l][i].timestamp_ms, tbuf, sizeof(tbuf));
            json_escape(lists[l][i].message, esc, sizeof(esc));
            // 预留 2 字节收尾 "]}" + 至少 1 字节逗号/条目
            if (JSON_LEFT() < 320) { truncated = true; break; }
            int n = snprintf(p, JSON_LEFT(),
                /* 字段名必须是 "message":
                 * 与 log_entry_t.message 一致, 也和前端 e.message 直觉对应。
                 * 曾因写成 "msg" 导致前端取不到值, 日志面板整列显示 undefined。 */
                "%s{\"time\":\"%s\",\"level\":%d,\"message\":\"%s\"}",
                first ? "" : ",", tbuf, (int)lists[l][i].level, esc);
            if (n < 0 || (size_t)n >= JSON_LEFT()) { truncated = true; break; }
            p += n;
            first = false;
        }
    }
#undef JSON_LEFT

    // 收尾 (确保有空间)
    if ((size_t)(p - json) + 3 < cap) {
        p += snprintf(p, 3, "]}");
    } else {
        strcpy(json + cap - 3, "]}");   // 极端情况: 直接覆盖末尾
    }
    (void)truncated;

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
    LOG_W("Web triggered server GPIO reset");
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

static void poweron_task(void *pv)
{
    ESP_LOGI(TAG, "Web 触发服务器开机");
    gpio_trigger_server_poweron(0);
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
    const char *str = (st == POWER_STATE_ON) ? "on" : "off";

    char json[256];
    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"gpio\":%d}",
        str, CONFIG_POWER_DETECT_GPIO_PIN);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

static esp_err_t handler_settings(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char interval_str[16] = "0";
        char timeout_str[16] = "0";
        httpd_query_key_value(query, "interval", interval_str, sizeof(interval_str));
        httpd_query_key_value(query, "timeout", timeout_str, sizeof(timeout_str));

        uint32_t interval = (uint32_t)atoi(interval_str);
        uint32_t timeout = (uint32_t)atoi(timeout_str);

        if (interval < 1) interval = 1;
        if (timeout < 60) timeout = 60;   // 10 分钟超时策略, 下限 60s
        // 上限钳制: 超大 interval 会在看门狗里 interval*1000 溢出 uint32
        if (interval > 86400) interval = 86400;
        if (timeout > 86400) timeout = 86400;

        watchdog_set_params(interval, timeout);
        nvs_save_heartbeat_params(interval, timeout);

        ESP_LOGI(TAG, "参数已更新: interval=%lu, timeout=%lu",
                 (unsigned long)interval, (unsigned long)timeout);
        LOG_I("Heartbeat params updated: %lus / %lus", (unsigned long)interval, (unsigned long)timeout);
    } else {
        // 之前 query 解析失败时静默跳过并返回 "已保存", 误导前端
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"缺少 interval/timeout 参数\"}", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"参数已保存\"}", -1);
    return ESP_OK;
}

static esp_err_t handler_change_password(httpd_req_t *req)
{
    if (!require_auth(req, true)) return ESP_OK;

    char query[256];
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
        // 只改用户名、新密码留空时保留原密码。
        // 之前会把空密码写进 NVS, 之后用脚本提交空密码即可登录。
        if (strlen(new_pass) == 0) {
            strcpy(new_pass, real_pass);
        }

        esp_err_t ret = nvs_save_credentials(new_user, new_pass);
        if (ret != ESP_OK) {
            httpd_resp_send_500(req);
            return ESP_OK;
        }

        LOG_I("Web credentials updated by %s", real_user);
        clear_login_cookie(req);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"凭据已更新，请重新登录\"}", -1);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"无变更\"}", -1);
    return ESP_OK;
}

// ==================== 固件 OTA 上传 ====================

static void firmware_reboot_task(void *pv)
{
    // 延迟重启, 确保 "刷写成功" 的 HTTP 响应已发回前端
    vTaskDelay(pdMS_TO_TICKS(2500));
    ESP_LOGI(TAG, "OTA rebooting...");
    LOG_W("OTA rebooting device");
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
    LOG_I("Firmware upload start: %ld bytes", (long)req->content_len);

    esp_err_t ret = ota_handle_raw_upload(req, (uint32_t)req->content_len);

    if (ret == ESP_OK) {
        LOG_I("Firmware flashed OK, scheduling reboot");
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

    ESP_LOGI(TAG, "Web 请求重置 WiFi (将恢复默认账号密码)");
    LOG_W("Web triggered WiFi reset (credentials -> default)");
    nvs_clear_wifi_config();   // 内部会调用 nvs_restore_default_credentials()

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"WiFi 已清除，正在重启...\"}", -1);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ==================== Dashboard (美化版) ====================

/* ---- {{KEY}} 占位符替换 (替代 printf, 避免 CSS 里的 % 被当作格式符) ---- */
static void ph_replace(char *buf, size_t buf_size, const char *key, const char *val)
{
    size_t klen = strlen(key), vlen = strlen(val);
    char *pos = buf;
    while ((pos = strstr(pos, key)) != NULL) {
        size_t used = (size_t)(pos - buf);
        size_t tail = strlen(pos + klen);
        if (used + vlen + tail + 1 > buf_size) return;   /* 空间不足则放弃, 保证不越界 */
        memmove(pos + vlen, pos + klen, tail + 1);       /* 连同结尾 '\0' 一起搬 */
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
    //       正确写法是 esp_app_get_description() (属于 esp_app_format 组件)。
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *version = (app_desc && strlen(app_desc->version) > 0) ? app_desc->version : "unknown";
    const char *build_date = (app_desc && strlen(app_desc->date) > 0) ? app_desc->date : "unknown";

    // 用 {{KEY}} 占位符做字符串替换 (不经 printf, 避免 CSS 中的 % 被当格式符)
    const char *tpl = dashboard_get_html();

    size_t cap = strlen(tpl) + 512;
    char *html = malloc(cap);
    if (!html) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }
    snprintf(html, cap, "%s", tpl);

    char v_interval[16], v_timeout[16];
    char v_hb[16], v_resp[16], v_to[16], v_reboot[16];
    snprintf(v_interval, sizeof(v_interval), "%lu", (unsigned long)hb_interval);
    snprintf(v_timeout,  sizeof(v_timeout),  "%lu", (unsigned long)hb_timeout);
    snprintf(v_hb,       sizeof(v_hb),       "%lu", (unsigned long)stats.heartbeat_count);
    snprintf(v_resp,     sizeof(v_resp),     "%lu", (unsigned long)stats.response_count);
    snprintf(v_to,       sizeof(v_to),       "%lu", (unsigned long)stats.timeout_count);
    snprintf(v_reboot,   sizeof(v_reboot),   "%lu", (unsigned long)uptime_get_reboots());

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
    // 栈大小: 默认 4096 不够用。
    // handler_dashboard 要 snprintf 渲染 ~22KB 的 HTML 模板 (栈上还有局部缓冲),
    // 历史版本在 8192 下仍会因 /api/logs 的日志数组撑爆栈而 panic
    // (Guru Meditation: Stack protection fault)。这里放大到 12288 留足余量。
    config.stack_size = 12288;
    config.max_uri_handlers = 32;
    // OTA 上传可能耗时较长, 适当放大请求超时
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
        { .uri = "/api/poweron",          .method = HTTP_POST, .handler = handler_poweron },
        { .uri = "/api/poweroff",         .method = HTTP_POST, .handler = handler_poweroff },
        { .uri = "/api/power-state",      .method = HTTP_GET,  .handler = handler_power_state },
        { .uri = "/api/settings",         .method = HTTP_POST, .handler = handler_settings },
        { .uri = "/api/change_password",  .method = HTTP_POST, .handler = handler_change_password },
        { .uri = "/api/firmware",         .method = HTTP_POST, .handler = handler_firmware },
        { .uri = "/api/ota/status",       .method = HTTP_GET,  .handler = handler_ota_status },
        { .uri = "/api/reset_wifi",       .method = HTTP_POST, .handler = handler_reset_wifi },
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
