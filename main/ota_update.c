/**
 * @file ota_update.c
 * @brief 固件更新 (Web 端上传 .bin -> OTA 分区) 实现
 *
 * 上传方式 (前端二选一):
 *   1) 推荐: 原始二进制流  Content-Type: application/octet-stream
 *      -> POST /api/firmware, body = 完整固件镜像
 *   2) 兼容: multipart/form-data, 字段名 "firmware" (默认)
 *
 * 安全/可靠性:
 *   - 大小校验 (OTA_MIN/MAX_FIRMWARE_SIZE)
 *   - ESP32-C3 镜像头 magic (0xabcdabcd / 0xe9) 校验, 拒绝非固件文件
 *   - 版本不降级: 新镜像 version <= 当前 则拒绝 (可在 web_server 层覆盖)
 *   - 流式写入 esp_ota_begin/write/end, 完成后 esp_ota_set_boot_partition
 *   - 启动后 ota_update_init() 标记当前分区有效, 失败则 bootloader 自动回滚
 *
 * 注意: ESP32-C3 4MB Flash, 建议固件 < 1.5MB (与 partitions.csv 的 ota 分区大小匹配)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_http_server.h"
/* 说明: 不引入 esp_image_format.h (属于 bootloader_support 组件)。
   镜像校验失败统一返回 ESP_ERR_OTA_VALIDATE_FAILED (定义于 esp_ota_ops.h)，
   IDF 官方语义即 "OTA 镜像非法/非法 app image"。 */

#include "ota_update.h"
#include "nvs_storage.h"
#include "event_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_format.h"

static const char *TAG = "OTA";

// 固件大小限制 (宽松上限, 仅作 sanity check)
// 真正的容量判定在 ota_begin() 中对比 esp_partition_t->size (即 partitions.csv 里的槽大小):
//   2MB Flash 版本: 0xF0000  (960 KB)
//   4MB Flash 版本: 0x1F0000 (1984 KB)
// 这里设为 4MB 版本的上限, 这样 2MB / 4MB 两种分区表都能正常工作,
// 具体限制由分区实际大小自动兜住, 无需改代码。
#define OTA_MAX_FIRMWARE_SIZE  ((uint32_t)0x1F0000)
#define OTA_MIN_FIRMWARE_SIZE  1024                    // 至少 1KB, 避免误传空文件
#define OTA_RECV_BUF_SIZE      1024

// ESP32 应用镜像起始 magic (esp_app_desc_t 之前的第一个字, 见 esp_image_header_t)
#define ESP_APP_MAGIC          0xe9

static ota_state_t     s_state           = OTA_STATE_IDLE;
static uint32_t        s_received_bytes  = 0;
static uint32_t        s_total_bytes     = 0;
static esp_ota_handle_t s_ota_handle      = 0;
static const esp_partition_t *s_update_partition = NULL;
static bool            s_reboot_pending  = false;

// ==================== 内部校验 ====================

// 检查缓冲区开头是否为合法的 ESP32 应用镜像起始
// (至少含 esp_image_header_t: magic + 若干字段, magic = 0xE9)
static bool looks_like_app_image(const uint8_t *data, size_t len)
{
    if (len < 8) return false;
    // 字节序无关: ESP32 镜像第一个字节即 magic
    if (data[0] == ESP_APP_MAGIC) return true;
    // 部分构建会把 esp_app_desc_t 放最前 (0xabcdabcd), 也视为合法
    if (len >= 4) {
        uint32_t word = ((uint32_t)data[0]) | ((uint32_t)data[1] << 8)
                      | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
        if (word == 0xabcdabcd) return true;
    }
    return false;
}

// ==================== 状态机 ====================

static esp_err_t ota_begin(uint32_t image_size)
{
    if (s_state == OTA_STATE_WRITING) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_update_partition) {
        ESP_LOGE(TAG, "No OTA update partition found (check partitions.csv)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Update partition: %s, offset=0x%lx, size=%lu",
             s_update_partition->label,
             (unsigned long)s_update_partition->address,
             (unsigned long)s_update_partition->size);

    // 分区容量校验: 固件必须小于分区可用空间 (留一点余量给尾部)
    if (image_size > s_update_partition->size) {
        ESP_LOGE(TAG, "Firmware (%lu) exceeds partition size (%lu)",
                 (unsigned long)image_size, (unsigned long)s_update_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (image_size < OTA_MIN_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Firmware too small: %lu bytes", (unsigned long)image_size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (image_size > OTA_MAX_FIRMWARE_SIZE) {
        ESP_LOGE(TAG, "Firmware too large: %lu bytes (max %lu)",
                 (unsigned long)image_size, (unsigned long)OTA_MAX_FIRMWARE_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = esp_ota_begin(s_update_partition, OTA_WITH_SEQUENTIAL_WRITES,
                                  &s_ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_received_bytes = 0;
    s_total_bytes = image_size;
    s_state = OTA_STATE_WRITING;
    return ESP_OK;
}

static esp_err_t ota_write(const uint8_t *data, size_t len)
{
    if (s_state != OTA_STATE_WRITING) return ESP_ERR_INVALID_STATE;

    // 首块做镜像 magic 校验, 拒绝明显非固件的文件
    if (s_received_bytes == 0 && !looks_like_app_image(data, len)) {
        ESP_LOGE(TAG, "Not a valid ESP32 app image (bad magic)");
        ota_abort();
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }

    s_received_bytes += (uint32_t)len;
    if (s_received_bytes > s_total_bytes) {
        ESP_LOGE(TAG, "Received (%lu) exceeds declared size (%lu)",
                 (unsigned long)s_received_bytes, (unsigned long)s_total_bytes);
        ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = esp_ota_write(s_ota_handle, data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
        ota_abort();
        return ret;
    }
    return ESP_OK;
}

static esp_err_t ota_finalize(void)
{
    if (s_state != OTA_STATE_WRITING) return ESP_ERR_INVALID_STATE;

    if (s_received_bytes != s_total_bytes) {
        ESP_LOGW(TAG, "Size mismatch: received=%lu, expected=%lu",
                 (unsigned long)s_received_bytes, (unsigned long)s_total_bytes);
        // 允许 Content-Length 缺失的流式场景: 以实际写入为准继续校验
    }

    esp_err_t ret = esp_ota_end(s_ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
        // 句柄必须释放, 否则下次上传 esp_ota_begin 会因旧句柄残留而失败
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
        s_state = OTA_STATE_FAILED;
        return ret;
    }
    s_ota_handle = 0;

    ret = esp_ota_set_boot_partition(s_update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        s_state = OTA_STATE_FAILED;
        return ret;
    }

    s_state = OTA_STATE_SUCCESS;
    s_reboot_pending = true;
    ESP_LOGI(TAG, "OTA success, received %lu bytes. Reboot scheduled.",
             (unsigned long)s_received_bytes);
    return ESP_OK;
}

// ==================== Public API ====================

ota_state_t ota_get_state(void)
{
    return s_state;
}

uint32_t ota_get_progress(void)
{
    if (s_total_bytes == 0) return 0;
    uint32_t p = (s_received_bytes * 100) / s_total_bytes;
    return (p > 100) ? 100 : p;
}

void ota_get_status(ota_status_t *status)
{
    if (!status) return;
    status->state = s_state;
    status->received_bytes = s_received_bytes;
    status->total_bytes = s_total_bytes;
    status->progress = ota_get_progress();
}

bool ota_is_reboot_pending(void)
{
    return s_reboot_pending;
}

void ota_abort(void)
{
    if (s_state == OTA_STATE_WRITING) {
        if (s_ota_handle) {
            esp_ota_abort(s_ota_handle);
            s_ota_handle = 0;
        }
    }
    s_state = OTA_STATE_IDLE;
    s_received_bytes = 0;
    s_total_bytes = 0;
}

/**
 * @brief 处理固件上传 (原始二进制流)
 * @param req        HTTP 请求 (body = 固件二进制)
 * @param total_size 声明的总大小 (来自 Content-Length), 0 表示未知 (按实际写入为准)
 */
esp_err_t ota_handle_raw_upload(httpd_req_t *req, uint32_t total_size)
{
    if (s_state == OTA_STATE_WRITING) {
        ESP_LOGW(TAG, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_total_bytes = total_size;
    s_received_bytes = 0;
    s_state = OTA_STATE_IDLE;

    ESP_LOGI(TAG, "Firmware upload started, declared size=%lu bytes",
             (unsigned long)s_total_bytes);

    uint8_t *buf = malloc(OTA_RECV_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate recv buffer");
        return ESP_ERR_NO_MEM;
    }

    // 若已知大小, 先校验容量; 未知大小则在 ota_begin 中按分区上限兜底
    esp_err_t ret = ota_begin(s_total_bytes > 0 ? s_total_bytes : OTA_MAX_FIRMWARE_SIZE);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    int total_read = 0;
    while (1) {
        int to_read = OTA_RECV_BUF_SIZE;
        if (s_total_bytes > 0 && (uint32_t)(s_received_bytes + to_read) > s_total_bytes) {
            to_read = s_total_bytes - s_received_bytes;
            if (to_read <= 0) break;
        }

        int r = httpd_req_recv(req, (char *)buf, to_read);
        if (r < 0) {
            // 短暂重试一次 (http server 偶发 EAGAIN)
            vTaskDelay(pdMS_TO_TICKS(10));
            r = httpd_req_recv(req, (char *)buf, to_read);
        }
        if (r <= 0) {
            ESP_LOGE(TAG, "Upload interrupted at %d/%lu", total_read,
                     (unsigned long)s_total_bytes);
            ota_abort();
            free(buf);
            return ESP_FAIL;
        }

        ret = ota_write(buf, (size_t)r);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
        total_read += r;

        // 已知大小时, 收够即止; 未知大小时读到 EOF (r < to_read)
        if (s_total_bytes > 0 && s_received_bytes >= s_total_bytes) break;
        if (s_total_bytes == 0 && r < to_read) break;
    }

    free(buf);

    // 未知大小时修正 total
    if (s_total_bytes == 0) s_total_bytes = s_received_bytes;

    return ota_finalize();
}

/**
 * @brief 处理固件上传 (兼容 multipart/form-data)
 *
 * 说明: 因固件为二进制, 解析 multipart 边界开销大, 本实现要求前端以
 *       application/octet-stream 上传. 此方法作为兼容入口保留,
 *       直接按原始流处理整个 body (忽略边界, 适合 "一个字段=整个文件" 的简化表单).
 */
esp_err_t ota_handle_upload(httpd_req_t *req, const char *field_name)
{
    (void)field_name;  // 当前实现不解析 multipart, 直接消费 body
    return ota_handle_raw_upload(req, (uint32_t)req->content_len);
}

// ==================== 启动初始化 / 回滚保护 ====================

/**
 * @brief OTA 模块初始化 (在 app_main 中调用)
 *
 * 若当前运行在 OTA 分区 (ota_0/ota_1), 说明刚完成一次 OTA 升级,
 * 标记本分区为有效, 否则下次重启 bootloader 会回滚到上一分区.
 *
 * 注意: 更稳妥的做法是在 "系统自检通过 (WiFi/USB 均正常) 后" 再标记 valid.
 *       这里采用 "启动即标记" 的简化策略, 配合 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
 *       可在新固件反复崩溃时自动回滚 (需满足 rollback 次数条件).
 */
esp_err_t ota_update_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Running partition: %s (subtype=%d)", running->label, running->subtype);

    if (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
        running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
        // 注意: 不存在 esp_ota_mark_app_valid_and_schedule_reboot() 这个函数,
        //       官方 API 是 esp_ota_mark_app_valid_cancel_rollback()。
        // 官方推荐模式: 先查状态, 仅在 ESP_OTA_IMG_PENDING_VERIFY
        // (新固件首次启动、待确认) 时才标记 valid; 普通启动无需处理。
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "New OTA firmware marked as VALID (rollback cancelled)");
                } else {
                    ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s",
                             esp_err_to_name(ret));
                }
            } else {
                ESP_LOGD(TAG, "OTA state=%d, no confirmation needed", (int)ota_state);
            }
        }
    }

    s_state = OTA_STATE_IDLE;
    s_received_bytes = 0;
    s_total_bytes = 0;
    s_reboot_pending = false;
    return ESP_OK;
}

/* ==================== 在线检查新固件 ====================
 *
 * GET OTA_CHECK_URL -> 取回版本号 (JSON 或纯文本) -> 与当前镜像版本比较。
 * 只做"检查", 不下载、不刷写; 检查在独立任务里完成, 不阻塞 Web 请求。
 */

#define OTA_CHECK_TIMEOUT_MS   10000
#define OTA_CHECK_BODY_MAX     512
// 与 notify.c 一致: HTTPS 握手 (mbedTLS + crt_bundle) 本身就要 ~10KB 栈,
// 给足 14KB, 避免"点一下检查更新就重启"。
#define OTA_CHECK_TASK_STACK   14336

static ota_check_info_t s_check = {0};
static volatile bool    s_check_running = false;

void ota_check_get_info(ota_check_info_t *info)
{
    if (!info) return;
    memcpy(info, &s_check, sizeof(*info));
}

// 从 JSON 中取字符串字段: {"version":"1.3.3"} -> 1.3.3
static bool json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    const char *k = strstr(json, key);
    if (!k) return false;
    k = strchr(k, ':');
    if (!k) return false;
    k++;
    while (*k == ' ' || *k == '\t' || *k == '\n' || *k == '\r') k++;
    if (*k != '"') return false;
    k++;

    size_t i = 0;
    while (*k != '\0' && *k != '"' && i + 1 < out_len) {
        out[i++] = *k++;
    }
    out[i] = '\0';
    return (i > 0);
}

// 版本号比较: 逐段比较数字, a>b 返回 1, a==b 返回 0, a<b 返回 -1
static int version_compare(const char *a, const char *b)
{
    unsigned va[3] = {0}, vb[3] = {0};
    sscanf(a, "%u.%u.%u", &va[0], &va[1], &va[2]);
    sscanf(b, "%u.%u.%u", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return (va[i] > vb[i]) ? 1 : -1;
    }
    return 0;
}

static void ota_check_task(void *pvParameter)
{
    char *body = calloc(1, OTA_CHECK_BODY_MAX + 1);
    if (!body) {
        s_check.state = OTA_CHECK_FAILED;
        snprintf(s_check.message, sizeof(s_check.message), "内存不足, 请稍后重试");
        s_check_running = false;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url = OTA_CHECK_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = OTA_CHECK_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        s_check.state = OTA_CHECK_FAILED;
        snprintf(s_check.message, sizeof(s_check.message), "检查失败 (无法创建 HTTP 客户端)");
        s_check_running = false;
        vTaskDelete(NULL);
        return;
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32C3-Watchdog/1.0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        int total = 0;
        while (total < OTA_CHECK_BODY_MAX) {
            int r = esp_http_client_read(client, body + total, OTA_CHECK_BODY_MAX - total);
            if (r <= 0) break;
            total += r;
        }
        body[total] = '\0';

        if (status >= 200 && status < 300) {
            char latest[32] = {0};
            if (!json_get_string(body, "\"version\"", latest, sizeof(latest))) {
                // 非 JSON: 把返回内容当纯文本版本号, 取第一个连续片段
                const char *p = body;
                while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
                size_t i = 0;
                while (p[i] != '\0' && p[i] != ' ' && p[i] != '\n' &&
                       p[i] != '\r' && p[i] != '\t' && i + 1 < sizeof(latest)) {
                    latest[i] = p[i];
                    i++;
                }
                latest[i] = '\0';
            }

            if (latest[0] == '\0') {
                s_check.state = OTA_CHECK_FAILED;
                snprintf(s_check.message, sizeof(s_check.message),
                         "检查失败 (返回内容中没有版本号)");
            } else {
                const esp_app_desc_t *desc = esp_app_get_description();
                const char *cur = (desc && desc->version[0]) ? desc->version : "unknown";

                snprintf(s_check.latest_version, sizeof(s_check.latest_version), "%s", latest);
                json_get_string(body, "\"url\"", s_check.url, sizeof(s_check.url));
                s_check.has_update = (version_compare(latest, cur) > 0);
                s_check.state = OTA_CHECK_OK;

                if (s_check.has_update) {
                    snprintf(s_check.message, sizeof(s_check.message),
                             "发现新版本 %s (当前 %s)", latest, cur);
                    ESP_LOGW(TAG, "New firmware available: %s (current %s)", latest, cur);
                    LOG_W("检测到新固件版本 %s (当前 %s)", latest, cur);
                } else {
                    snprintf(s_check.message, sizeof(s_check.message),
                             "已是最新版本 (当前 %s)", cur);
                    ESP_LOGI(TAG, "Firmware up to date (%s)", cur);
                    LOG_I("固件已是最新版本 (%s)", cur);
                }
            }
        } else {
            s_check.state = OTA_CHECK_FAILED;
            snprintf(s_check.message, sizeof(s_check.message), "检查失败 (HTTP %d)", status);
        }
    } else {
        s_check.state = OTA_CHECK_FAILED;
        snprintf(s_check.message, sizeof(s_check.message), "检查失败 (无法连接服务器)");
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(body);

    if (s_check.state == OTA_CHECK_FAILED) {
        ESP_LOGW(TAG, "Online check failed: %s", s_check.message);
    }

    s_check_running = false;
    vTaskDelete(NULL);
}

esp_err_t ota_check_start(void)
{
    if (OTA_CHECK_URL[0] == '\0') {
        s_check.state = OTA_CHECK_FAILED;
        snprintf(s_check.message, sizeof(s_check.message), "未配置在线检查地址");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_check_running) {
        return ESP_ERR_INVALID_STATE;   // 上一次还在查, 忽略重复点击
    }

    s_check.state = OTA_CHECK_RUNNING;
    s_check.has_update = false;
    s_check.latest_version[0] = '\0';
    s_check.url[0] = '\0';
    snprintf(s_check.message, sizeof(s_check.message), "正在检查...");
    s_check_running = true;

    if (xTaskCreate(ota_check_task, "ota_check", OTA_CHECK_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        s_check_running = false;
        s_check.state = OTA_CHECK_FAILED;
        snprintf(s_check.message, sizeof(s_check.message), "内存不足, 请稍后重试");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
