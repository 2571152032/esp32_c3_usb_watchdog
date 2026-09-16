/**
 * @file watchdog.c
 * @brief USB 看门狗检测逻辑
 *
 * 策略 (v2):
 *  - 每 heartbeat_interval_s 发送一次 "ZAIMA\n"
 *  - 超过 heartbeat_timeout_s 未收到响应 -> 判定超时
 *  - 连续超时达阈值 -> 判定宕机, 触发 GPIO 复位
 *  - 指数退避: 重启后等待 30/60/120/240/300s 再继续监控
 *  - 最大重启: 1 小时内超过 CONFIG_WD_MAX_REBOOTS_PER_HOUR 次则停止 (防死循环)
 *  - 开机宽限期: 服务器刚重启的 CONFIG_WD_BOOT_GRACE_PERIOD_S 秒内不计超时
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "watchdog.h"
#include "usb_device.h"
#include "gpio_control.h"
#include "uptime.h"
#include "event_log.h"

#define TAG "WATCHDOG"

#define DEFAULT_HEARTBEAT_INTERVAL  60   // 秒 (每 60 秒发送一次 ZAIMA)
#define DEFAULT_HEARTBEAT_TIMEOUT   600  // 秒 (10 分钟无响应即判定宕机)
#define MAX_CONSECUTIVE_TIMEOUTS    1    // 达超时阈值即判定宕机
#define WD_MAX_INTERVAL_S           86400  // 参数上限 (interval*1000 参与 uint32 运算, 需防溢出)
#define WD_MAX_TIMEOUT_S            86400
#define STABLE_RESET_MS             (5 * 60 * 1000UL)  // 连续稳定 5 分钟才重置退避计数

static struct {
    bool running;
    uint32_t heartbeat_interval_s;
    uint32_t heartbeat_timeout_s;
    uint32_t last_heartbeat_ms;
    uint32_t last_response_ms;
    uint32_t heartbeat_count;
    uint32_t response_count;
    uint32_t timeout_count;
    uint32_t consecutive_timeouts;
    watchdog_state_t state;
    TaskHandle_t task_handle;
    void (*server_down_callback)(void);

    // === 策略增强 ===
    uint32_t reboot_count_in_hour;    // 1 小时窗口内重启次数
    uint32_t window_start_ms;         // 窗口起始
    uint32_t consecutive_reboots;     // 连续重启次数 (指数退避)
    uint32_t boot_grace_until_ms;     // 开机宽限期结束时刻
    uint32_t stable_since_ms;         // 恢复后连续无超时的起始时刻 (0=未开始计时)
} s_wd = {0};

// ==================== 趋势环形缓冲 ====================
static struct {
    trend_point_t points[WD_TREND_MAX_POINTS];
    uint32_t head;
    uint32_t count;
} s_trend = {0};

static void trend_push(trend_point_t p)
{
    s_trend.points[s_trend.head] = p;
    s_trend.head = (s_trend.head + 1) % WD_TREND_MAX_POINTS;
    if (s_trend.count < WD_TREND_MAX_POINTS) s_trend.count++;
}

uint32_t watchdog_get_trend(trend_point_t *points, uint32_t max)
{
    if (!points || max == 0) return 0;
    uint32_t start = (s_trend.count < WD_TREND_MAX_POINTS) ? 0 : s_trend.head;
    uint32_t copy  = (s_trend.count < max) ? s_trend.count : max;
    for (uint32_t i = 0; i < copy; i++) {
        points[i] = s_trend.points[(start + i) % WD_TREND_MAX_POINTS];
    }
    return copy;
}

uint32_t watchdog_get_trend_count(void) { return s_trend.count; }

uint32_t watchdog_get_retry_delay(void)
{
    // 30 -> 60 -> 120 -> 240 -> 300 (封顶)
    static const uint32_t delays[] = {30, 60, 120, 240, 300};
    uint32_t idx = (s_wd.consecutive_reboots < 5) ? s_wd.consecutive_reboots : 4;
    return delays[idx];
}

// ==================== 内部状态重置 ====================
static void watchdog_reset_state_internal(void)
{
    s_wd.consecutive_timeouts = 0;
    s_wd.last_response_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_wd.state == WD_STATE_SERVER_DOWN) {
        s_wd.state = WD_STATE_HEALTHY;
        LOG_I("服务器连接已恢复");
    }
}

// USB 回调: 收到响应
static void usb_packet_handler(const usb_packet_t *packet)
{
    if (!packet) return;
    if (packet->cmd == USB_CMD_ACK || packet->cmd == USB_CMD_PING) {
        watchdog_notify_response();
    } else if (packet->cmd == USB_CMD_BYE) {
        ESP_LOGW(TAG, "Server sent BYE");
        LOG_W("服务器发送了 BYE 信号");
        s_wd.state = WD_STATE_SERVER_DOWN;
    }
}

static void watchdog_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Watchdog task started (interval=%lus, timeout=%lus)",
             s_wd.heartbeat_interval_s, s_wd.heartbeat_timeout_s);
    LOG_I("看门狗已启动: 心跳间隔 %lu 秒, 超时 %lu 秒",
          (unsigned long)s_wd.heartbeat_interval_s, (unsigned long)s_wd.heartbeat_timeout_s);

    vTaskDelay(pdMS_TO_TICKS(5000));

    s_wd.last_response_ms = (uint32_t)(esp_timer_get_time() / 1000);
    usb_register_packet_callback(usb_packet_handler);

    while (s_wd.running) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // 检查 USB 是否已连接
        if (!usb_is_connected()) {
            if (s_wd.state != WD_STATE_IDLE) {
                s_wd.state = WD_STATE_IDLE;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 开机宽限期: 服务器刚重启, 暂不判定超时
        if (s_wd.boot_grace_until_ms > 0 && now_ms < s_wd.boot_grace_until_ms) {
            // 宽限期内持续刷新 last_response, 避免宽限期结束瞬间立即超时
            s_wd.last_response_ms = now_ms;
            s_wd.consecutive_timeouts = 0;
        } else if (s_wd.boot_grace_until_ms > 0 && now_ms >= s_wd.boot_grace_until_ms) {
            // 宽限期刚结束
            LOG_I("开机宽限期已结束, 恢复正常监控");
            s_wd.boot_grace_until_ms = 0;
        }

        // 注: 响应统一由 USB 回调路径处理 (usb_rx_task -> usb_packet_handler ->
        //     watchdog_notify_response)。这里不再轮询 usb_receive_packet() ——
        //     之前两条路径并发读同一个 USB 驱动缓冲、共用同一个静态 s_rx_buf,
        //     存在数据竞争, 且统计口径不一致。

        // 周期性发送心跳
        if (now_ms - s_wd.last_heartbeat_ms >= s_wd.heartbeat_interval_s * 1000) {
            usb_send_heartbeat();
            s_wd.heartbeat_count++;
            s_wd.last_heartbeat_ms = now_ms;

            uint32_t elapsed_s = (now_ms - s_wd.last_response_ms) / 1000;

            if (elapsed_s > s_wd.heartbeat_timeout_s) {
                s_wd.consecutive_timeouts++;
                s_wd.timeout_count++;
                s_wd.stable_since_ms = 0;   // 出现超时, 稳定期重新计时
                ESP_LOGW(TAG, "Heartbeat timeout #%lu (elapsed=%lus, limit=%lus)",
                         (unsigned long)s_wd.consecutive_timeouts,
                         (unsigned long)elapsed_s, (unsigned long)s_wd.heartbeat_timeout_s);
                LOG_W("心跳超时 #%lu (已等待 %lu 秒)",
                      (unsigned long)s_wd.consecutive_timeouts, (unsigned long)elapsed_s);

                if (s_wd.consecutive_timeouts >= MAX_CONSECUTIVE_TIMEOUTS) {
                    s_wd.state = WD_STATE_SERVER_DOWN;
                    ESP_LOGE(TAG, "SERVER DOWN DETECTED!");
                    LOG_E("检测到服务器宕机 -> GPIO 复位 (脉冲 500 毫秒)");

                    trend_push(TREND_DOWN);
                    uptime_inc_reboots();

                    // 检查 1 小时窗口
                    if (now_ms - s_wd.window_start_ms > 3600000UL) {
                        s_wd.window_start_ms = now_ms;
                        s_wd.reboot_count_in_hour = 0;
                    }
                    s_wd.reboot_count_in_hour++;
                    s_wd.consecutive_reboots++;

                    if (s_wd.reboot_count_in_hour > CONFIG_WD_MAX_REBOOTS_PER_HOUR) {
                        ESP_LOGE(TAG, "MAX REBOOTS (%d/hour) REACHED - stopping watchdog",
                                 CONFIG_WD_MAX_REBOOTS_PER_HOUR);
                        LOG_F("看门狗已停止: 1 小时内重启次数过多 (%d 次), 需人工介入",
                              CONFIG_WD_MAX_REBOOTS_PER_HOUR);
                        gpio_set_led_state(LED_BLINK_FAST);  // 快闪 = 需人工介入
                        s_wd.running = false;
                        break;
                    }

                    if (s_wd.server_down_callback) {
                        s_wd.server_down_callback();
                    }
                    gpio_trigger_server_reset(500);

                    // 指数退避等待
                    uint32_t wait_s = watchdog_get_retry_delay();
                    ESP_LOGI(TAG, "Waiting %lus for server reboot (attempt #%lu, backoff)",
                             (unsigned long)wait_s, (unsigned long)s_wd.consecutive_reboots);
                    LOG_W("等待服务器重启 %lu 秒 (退避, 第 %lu 次)",
                          (unsigned long)wait_s, (unsigned long)s_wd.consecutive_reboots);

                    // 分段延时, 期间允许停止
                    for (uint32_t w = 0; w < wait_s && s_wd.running; w++) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }

                    // 设置开机宽限期
                    s_wd.boot_grace_until_ms = (uint32_t)(esp_timer_get_time() / 1000)
                                               + (CONFIG_WD_BOOT_GRACE_PERIOD_S * 1000);
                    LOG_I("开机宽限期 %u 秒", CONFIG_WD_BOOT_GRACE_PERIOD_S);

                    watchdog_reset_state();
                    s_wd.state = WD_STATE_HEALTHY;
                } else {
                    s_wd.state = WD_STATE_WARNING;
                    trend_push(TREND_TIMEOUT);
                }
            } else {
                s_wd.state = WD_STATE_HEALTHY;
            }
            ESP_LOGD(TAG, "Heartbeat #%lu sent (state=%d)",
                     (unsigned long)s_wd.heartbeat_count, s_wd.state);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "Watchdog task stopped");
    s_wd.task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t watchdog_init(void)
{
    ESP_LOGI(TAG, "Initializing watchdog...");
    memset(&s_wd, 0, sizeof(s_wd));
    s_wd.heartbeat_interval_s = DEFAULT_HEARTBEAT_INTERVAL;
    s_wd.heartbeat_timeout_s = DEFAULT_HEARTBEAT_TIMEOUT;
    s_wd.state = WD_STATE_IDLE;
    s_wd.window_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return ESP_OK;
}

esp_err_t watchdog_start(uint32_t heartbeat_interval_ms, uint32_t timeout_ms)
{
    if (s_wd.running) {
        ESP_LOGW(TAG, "Watchdog already running");
        return ESP_OK;
    }
    if (heartbeat_interval_ms > 0 || timeout_ms > 0) {
        // 统一经 watchdog_set_params 处理 (含上限钳制)
        uint32_t i_s = heartbeat_interval_ms / 1000;
        uint32_t t_s = timeout_ms / 1000;
        if (heartbeat_interval_ms > 0 && i_s == 0) i_s = 1;
        if (timeout_ms > 0 && t_s == 0) t_s = 1;
        watchdog_set_params(i_s, t_s);
    }

    s_wd.running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        watchdog_task, "watchdog_task", 4096, NULL, 3, &s_wd.task_handle, 0);
    if (ret != pdPASS) {
        s_wd.running = false;
        ESP_LOGE(TAG, "Failed to create watchdog task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void watchdog_stop(void)
{
    s_wd.running = false;
    if (s_wd.task_handle) {
        vTaskDelete(s_wd.task_handle);
        s_wd.task_handle = NULL;
    }
}

void watchdog_reset_state(void) { watchdog_reset_state_internal(); }
void watchdog_reset(void)       { watchdog_reset_state_internal(); }

bool watchdog_is_server_down(void)
{
    return (s_wd.state == WD_STATE_SERVER_DOWN);
}

watchdog_state_t watchdog_get_state(void) { return s_wd.state; }

void watchdog_get_stats(watchdog_stats_t *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    stats->state = s_wd.state;
    stats->last_heartbeat_time = s_wd.last_heartbeat_ms;
    stats->last_response_time  = s_wd.last_response_ms;
    stats->heartbeat_count = s_wd.heartbeat_count;
    stats->response_count  = s_wd.response_count;
    stats->timeout_count   = s_wd.timeout_count;
    stats->consecutive_timeouts = s_wd.consecutive_timeouts;
}

void watchdog_reset_stats(void)
{
    s_wd.heartbeat_count      = 0;
    s_wd.response_count       = 0;
    s_wd.timeout_count        = 0;
    s_wd.consecutive_timeouts = 0;
    s_wd.last_heartbeat_ms    = 0;
    s_wd.last_response_ms     = 0;
    ESP_LOGI(TAG, "Watchdog stats reset");
    LOG_I("心跳统计已清零");
}

void watchdog_notify_response(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_wd.last_response_ms = now_ms;
    s_wd.response_count++;
    s_wd.consecutive_timeouts = 0;

    if (s_wd.state == WD_STATE_WARNING || s_wd.state == WD_STATE_SERVER_DOWN) {
        s_wd.state = WD_STATE_HEALTHY;
        LOG_I("服务器已响应, 连接恢复");
    }

    // 连续稳定 (无超时) 超过 STABLE_RESET_MS 才重置退避计数。
    // 之前收到任意一个 ACK 就立即清零, 服务器反复 "假活" 时指数退避完全失效。
    if (s_wd.consecutive_reboots > 0) {
        if (s_wd.stable_since_ms == 0) {
            s_wd.stable_since_ms = now_ms;
        } else if (now_ms - s_wd.stable_since_ms >= STABLE_RESET_MS) {
            ESP_LOGI(TAG, "Server stable for %us, resetting consecutive reboot count",
                     (unsigned)(STABLE_RESET_MS / 1000));
            LOG_I("服务器稳定运行, 已重置连续重启计数");
            s_wd.consecutive_reboots = 0;
            s_wd.stable_since_ms = 0;
        }
    }
}

void watchdog_set_params(uint32_t interval_s, uint32_t timeout_s)
{
    // 上限钳制: interval*1000 参与 uint32 运算, 超过 ~4294967 会溢出
    if (interval_s > 0) {
        s_wd.heartbeat_interval_s = (interval_s > WD_MAX_INTERVAL_S) ? WD_MAX_INTERVAL_S : interval_s;
    }
    if (timeout_s > 0) {
        s_wd.heartbeat_timeout_s = (timeout_s > WD_MAX_TIMEOUT_S) ? WD_MAX_TIMEOUT_S : timeout_s;
    }
    ESP_LOGI(TAG, "Watchdog params: interval=%lus, timeout=%lus",
             (unsigned long)s_wd.heartbeat_interval_s, (unsigned long)s_wd.heartbeat_timeout_s);
}

void watchdog_get_params(uint32_t *interval_s, uint32_t *timeout_s)
{
    if (interval_s) *interval_s = s_wd.heartbeat_interval_s;
    if (timeout_s) *timeout_s = s_wd.heartbeat_timeout_s;
}

void watchdog_register_server_down_callback(void (*cb)(void))
{
    s_wd.server_down_callback = cb;
}
