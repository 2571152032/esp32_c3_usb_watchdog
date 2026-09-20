/**
 * @file smart_config.c
 * @brief 配网实现 (AP 模式 + 强制门户 + 手动/扫描选择 SSID)
 *
 * 流程：
 *  1. 启动 SoftAP (Watchdog-AP / 12345678)
 *  2. 启动强制门户 DNS 劫持 (captive_portal: 所有域名 -> AP 自身 IP)
 *  3. 启动 HTTP 服务器 (端口 80)，注册各系统探测 URL
 *  4. GET  /connect  -> 返回配网表单 HTML
 *     GET  /api/scan -> 扫描附近 WiFi (JSON)
 *     POST /connect  -> 解析表单 (ssid & password)，保存到 NVS，重启
 *     *             -> 其余一切请求 302 到配网首页
 *  5. 手机/电脑一连上热点就会自动弹出配网页面 (强制门户)
 *  6. 重启后进入正常模式，读取 NVS 连接 WiFi
 *
 * 提交方式：application/x-www-form-urlencoded
 * 解析方式：httpd_query_key_value (最稳定，不依赖 JSON)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
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
#include "captive_portal.h"
#include "nvs_storage.h"
#include "wizard.html.h"

#define TAG "SMART_CFG"

// AP 配置
#define AP_SSID             "Watchdog-AP"
#define AP_PASSWORD         "12345678"
#define AP_CHANNEL          1
#define AP_MAX_CONNECTIONS  4
#define CONFIG_TIMEOUT_MS   (5 * 60 * 1000)  // 5 分钟超时

// WiFi 扫描
#define SCAN_MAX_AP         20      // 最多返回 20 个热点
#define SCAN_JSON_SIZE      2048    // 响应 JSON 缓冲 (堆分配, 不占栈)

static httpd_handle_t s_server = NULL;
static smart_config_state_t s_state = SC_STATE_IDLE;

// 302 重定向目标。必须是绝对地址: Android 的强制门户检测会拿
// generate_204 响应里的 Location 去打开登录页, 相对路径会被拼到
// connectivitycheck.gstatic.com 上, 结果打不开。
static char s_redirect_url[32] = "http://192.168.4.1/";

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
    // iOS CNA 窗口会缓存页面, 不加 no-store 时二次配网可能拿到旧页面
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

/* ---------- 强制门户 (Captive Portal) ---------- */

// 302 到配网首页 —— Android / Windows 的连通性探测走这里。
// 返回 302 (而不是探测期待的 204/200) 正是"告诉系统: 这是个需要登录的
// 网络", 系统随后会弹通知 / 自动打开浏览器, 用户点开就是配网页。
static esp_err_t handler_portal_redirect(httpd_req_t *req)
{
    ESP_LOGD(TAG, "Captive probe hit: %s -> 302 %s", req->uri, s_redirect_url);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_redirect_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// iOS / macOS: Captive Network Assistant (CNA) 对 302 的处理不稳定,
// 直接给它 200 + 正常 HTML 页面, CNA 小窗口里就会显示配网页面。
// (系统判断是否"有网"的依据是响应体里是否含 "Success" 字样, 我们页面里没有)
static esp_err_t handler_portal_page(httpd_req_t *req)
{
    ESP_LOGD(TAG, "Captive probe hit (Apple): %s -> serve page", req->uri);
    return handler_get_page(req);
}

/* ---------- WiFi 扫描 ---------- */

/* JSON 字符串转义: 扫描到的 SSID 是用户输入, 可能含引号/反斜杠/控制字符,
 * 不转义会生成非法 JSON, 前端 r.json() 直接抛异常、列表空白。 */
static size_t json_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        if (o + 2 >= cap) {
            break;
        }
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                // 控制字符直接丢弃; 其余 (含 UTF-8 中文) 原样保留
                if (c >= 0x20) {
                    out[o++] = (char)c;
                }
        }
    }
    out[o] = '\0';
    return o;
}

// GET /api/scan -> 扫描附近 WiFi, 返回 {"ok":true,"list":[{"ssid":..,"rssi":..,"sec":..}]}
//
// 扫描是阻塞的 (~1.5s)。实测用户连点"重新扫描"会并发打进来, 每次都触发一次
// 全信道扫描并刷一屏日志, 故加 busy 标志做防重入。
static volatile bool s_scan_busy = false;

static esp_err_t handler_scan(httpd_req_t *req)
{
    if (s_scan_busy) {
        const char *busy = "{\"ok\":false,\"message\":\"正在扫描中, 请稍候\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, busy, strlen(busy));
        return ESP_OK;
    }
    s_scan_busy = true;

    wifi_ap_record_t *aps = (wifi_ap_record_t *)calloc(SCAN_MAX_AP, sizeof(wifi_ap_record_t));
    char *json = (char *)calloc(1, SCAN_JSON_SIZE);
    if (!aps || !json) {
        free(aps);
        free(json);
        s_scan_busy = false;
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    wifi_scan_config_t cfg = {0};
    cfg.show_hidden = false;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    // 缩短每信道驻留时间: 全信道扫太久会让 AP 侧的手机明显卡顿,
    // 100/300ms 足够收到绝大多数家用路由的 beacon/probe response
    cfg.scan_time.active.min = 100;
    cfg.scan_time.active.max = 300;

    const char *fail = "{\"ok\":false,\"message\":\"扫描失败, 请手动输入 WiFi 名称\"}";

    // 阻塞式扫描。httpd 任务没有注册 Task WDT, 短时阻塞安全。
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        wifi_mode_t m = WIFI_MODE_NULL;
        esp_wifi_get_mode(&m);
        ESP_LOGW(TAG, "wifi scan failed: %s (wifi mode=%d, AP=%d APSTA=%d)",
                 esp_err_to_name(err), (int)m, (int)WIFI_MODE_AP, (int)WIFI_MODE_APSTA);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, fail, strlen(fail));
        free(aps);
        free(json);
        s_scan_busy = false;
        return ESP_OK;
    }

    uint16_t num = SCAN_MAX_AP;
    if (esp_wifi_scan_get_ap_records(&num, aps) != ESP_OK) {
        num = 0;
    }

    // 按信号强度降序 + 同名去重 (保留最强的那个), 前端就按这个顺序展示
    int n = (num > SCAN_MAX_AP) ? SCAN_MAX_AP : num;
    for (int i = 1; i < n; i++) {
        wifi_ap_record_t key = aps[i];
        int j = i - 1;
        while (j >= 0 && aps[j].rssi < key.rssi) {
            aps[j + 1] = aps[j];
            j--;
        }
        aps[j + 1] = key;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int k = 0; k < m; k++) {
            if (strncmp((const char *)aps[i].ssid, (const char *)aps[k].ssid,
                        sizeof(aps[i].ssid)) == 0) {
                dup = true;
                break;
            }
        }
        if (dup || aps[i].ssid[0] == '\0') {
            continue;
        }
        aps[m++] = aps[i];
    }

    int p = 0;
    int cap = SCAN_JSON_SIZE;
#define JSON_LEFT() ((p >= 0 && p < cap) ? (cap - p) : 0)
    int w = snprintf(json, cap, "{\"ok\":true,\"list\":[");
    p = (w > 0 && w < cap) ? w : 0;
    for (int i = 0; i < m; i++) {
        if (JSON_LEFT() < 96) {          // 空间不足就截断, 绝不越界
            break;
        }
        char esc[64];
        json_escape(esc, sizeof(esc), (const char *)aps[i].ssid);
        // ⚠ snprintf 返回的是"本该写入的长度", 被截断时它大于实际写入量。
        //   若直接 p += 返回值, p 会越过 cap, 之后 cap-p 下溢成巨大值 -> 越界写。
        w = snprintf(json + p, JSON_LEFT(), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"sec\":%d}",
                     (i ? "," : ""), esc, aps[i].rssi, (int)aps[i].authmode);
        if (w <= 0) {
            break;
        }
        if (w >= JSON_LEFT()) {
            p = cap - 1;                 // 写满了, 停在这里由下面收尾
            break;
        }
        p += w;
    }
    if (JSON_LEFT() >= 3) {
        snprintf(json + p, JSON_LEFT(), "]}");
    } else {
        json[cap - 1] = '\0';            // 宁可截断, 也保证字符串有终止符
    }
#undef JSON_LEFT

    ESP_LOGI(TAG, "Scan done: %d AP(s)", m);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, json, strlen(json));

    free(aps);
    free(json);
    s_scan_busy = false;
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

/* 各系统的"网络连通性探测"地址。
 *
 * 系统连上 WiFi 后会访问自家的一个固定 URL 判断能不能上网:
 *   能拿到预期内容 -> 有网, 什么都不做
 *   拿到别的 / 被重定向 -> 判定为强制门户, 自动弹出登录(配网)页面
 *
 * 配上 DNS 劫持 (captive_portal.c 把所有域名解析成 192.168.4.1),
 * 这些请求都会落到本机, 于是实现"连上热点自动跳配网页"。
 *
 * ⚠ 末尾的 '*' 不是可选的 —— 实测 Android 请求的是
 *     /generate_204_15267541384063288033   (随机数字后缀)
 *     /generate_204_e46d7be6-f880-42d9-90de-2c59f4235b4b  (UUID 后缀)
 *   只注册 "/generate_204" 精确匹配抓不到, 必须前缀通配。
 *   这也要求 config.uri_match_fn = httpd_uri_match_wildcard (IDF v6.1 默认为 NULL)。
 */
static const char *const s_probe_redirect[] = {
    // Android
    "/generate_204*",
    "/gen_204*",
    "/mobile/status.php*",
    // Windows
    "/ncsi.txt*",
    "/connecttest.txt*",
    "/redirect*",
    "/fwlink*",
    // 其它常见
    "/check_network_status.txt*",
    "/network_status.txt*",
    "/success.txt*",
    "/kindle-wifi*",
    "/wifiredirect.html*",
};

// Apple 系: 返回 200 + HTML 页面 (CNA 窗口对 302 支持不好)
static const char *const s_probe_page[] = {
    "/hotspot-detect*",
    "/library/test/success.html*",
    "/success.html*",
};

// 启动 HTTP 服务器
static esp_err_t start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    // ⚠ 关键: IDF v6.1 的 HTTPD_DEFAULT_CONFIG() 里 uri_match_fn = NULL,
    //   即精确字符串比较, 那样 '*' 只是个普通字符, 通配路由完全不生效
    //   (实测日志: URI '/generate_204_15267541384063288033' not found)。
    //   必须显式开启通配匹配, 探测路由的 '*' 才算通配符。
    config.uri_match_fn = httpd_uri_match_wildcard;
    // 5 个基础路由 + 13 个重定向探测 + 3 个 Apple 探测 + 1 个通配, 给足余量
    config.max_uri_handlers = 32;
    // 302 要带 Location / Cache-Control, 默认 8 个头略紧, 与 web_server 保持一致
    config.max_resp_headers = 16;
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
    // GET /api/scan 扫描附近 WiFi
    httpd_uri_t uri_scan = {
        .uri = "/api/scan",
        .method = HTTP_GET,
        .handler = handler_scan,
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
    httpd_register_uri_handler(s_server, &uri_scan);
    httpd_register_uri_handler(s_server, &uri_favicon);

    // 强制门户探测地址 (返回 302)
    for (size_t i = 0; i < sizeof(s_probe_redirect) / sizeof(s_probe_redirect[0]); i++) {
        httpd_uri_t u = {
            .uri = s_probe_redirect[i],
            .method = HTTP_GET,
            .handler = handler_portal_redirect,
        };
        if (httpd_register_uri_handler(s_server, &u) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register probe: %s", s_probe_redirect[i]);
        }
    }

    // 强制门户探测地址 (Apple, 返回页面)
    for (size_t i = 0; i < sizeof(s_probe_page) / sizeof(s_probe_page[0]); i++) {
        httpd_uri_t u = {
            .uri = s_probe_page[i],
            .method = HTTP_GET,
            .handler = handler_portal_page,
        };
        if (httpd_register_uri_handler(s_server, &u) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register probe: %s", s_probe_page[i]);
        }
    }

    // 兜底通配: DNS 把所有域名都指向本机, 用户随手访问 http://任意网址
    // 都会打到这里, 一律 302 到配网首页。
    //
    // ⚠ httpd_find_uri_handler() 按【注册顺序】返回第一个匹配的 handler
    //   (不是"最长匹配"), 所以:
    //     - '*' 必须最后注册, 否则它会吃掉后面所有路由;
    //     - 而且注册 '*' 之后就不能再注册任何 handler 了
    //       (httpd_register_uri_handler 内部会先用新 URI 去查重, '*' 会
    //        匹配一切, 直接返回 ESP_ERR_HTTPD_HANDLER_EXISTS)。
    // ⚠ method 必须是 HTTP_ANY, 不能只写 HTTP_GET。
    //   DNS 把一切域名都指到本机, 手机上各 App 的后台请求会带着各种 method
    //   打进来 (实测: 阿里 AMDC 的 POST /amdc/mobileDispatch?appkey=...)。
    //   只注册 GET 的话这些请求会走 405, 刷一屏
    //     httpd_uri: Method '3' not allowed for URI '/amdc/mobileDispatch?...'
    //   强制门户本就该兜住所有 method, 一律 302。
    //   /connect 的 POST 已先注册, 不会被这里抢走。
    httpd_uri_t uri_wildcard = {
        .uri = "*",
        .method = HTTP_ANY,
        .handler = handler_portal_redirect,
    };
    ret = httpd_register_uri_handler(s_server, &uri_wildcard);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register wildcard route: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Web server started on port 80 (captive portal enabled)");
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

    // 【为什么配网期要用 AP+STA 而不是纯 AP】
    // 实测: 纯 AP 模式下 esp_wifi_scan_start() 直接返回 ESP_FAIL
    //   W SMART_CFG: wifi scan failed: ESP_FAIL
    // 也就是"扫描附近 WiFi"这个功能在 WIFI_MODE_AP 下根本不可用。
    // 故以 WIFI_MODE_APSTA 启动: STA 接口只用来扫描, 不配置 SSID 也不联网,
    // 内存代价很小 (配网结束必定 esp_restart, 不会遗留到正常运行态)。
    if (esp_netif_create_default_wifi_sta() == NULL) {
        ESP_LOGE(TAG, "Failed to create default netif for STA");
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

    // APSTA: STA 侧不设任何配置 (不会去连 AP), 仅提供扫描能力
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 取 AP 真实 IP 拼重定向地址 (默认 192.168.4.1, 但改过 netif 配置时以实际为准)
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(ap_netif, &info) == ESP_OK && info.ip.addr != 0) {
            uint32_t a = info.ip.addr;                 // 网络字节序: 低字节 = 第 1 段
            snprintf(s_redirect_url, sizeof(s_redirect_url), "http://%u.%u.%u.%u/",
                     (unsigned)(a & 0xFF), (unsigned)((a >> 8) & 0xFF),
                     (unsigned)((a >> 16) & 0xFF), (unsigned)((a >> 24) & 0xFF));
        }
    }

    ESP_LOGI(TAG, "AP started: SSID=%s, Password=%s", AP_SSID, AP_PASSWORD);
    ESP_LOGI(TAG, "Portal URL: %s", s_redirect_url);
    return ESP_OK;
}

esp_err_t smart_config_start(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Config Mode (AP) Started");
    ESP_LOGI(TAG, "  AP: %s", AP_SSID);
    ESP_LOGI(TAG, "  Password: %s", AP_PASSWORD);
    ESP_LOGI(TAG, "  Connect WiFi, portal page should pop up automatically");
    ESP_LOGI(TAG, "========================================");

    set_state(SC_STATE_AP_STARTED);

    esp_err_t ret = start_ap_mode();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AP mode");
        return ret;
    }

    // 启动强制门户 DNS 劫持: 必须在 AP netif 建好之后 (要拿 AP 自身 IP)。
    // 失败不影响手动配网 (用户还能自己打开 192.168.4.1), 故只告警不返回错误。
    if (captive_portal_start() != ESP_OK) {
        ESP_LOGW(TAG, "Captive portal DNS not started, manual open required");
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
        // 先查当前任务是否已注册: TWDT 不可用时直接 reset 会每秒刷一条错误日志
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }

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
    // 先停 DNS 劫持, 再停 HTTP 服务, 避免残留把客户端的域名继续指向本机
    captive_portal_stop();

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

smart_config_state_t smart_config_get_state(void)
{
    return s_state;
}
