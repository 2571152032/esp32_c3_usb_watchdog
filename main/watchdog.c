/**
 * @file watchdog.c
 * @brief USB 看门狗检测逻辑
 *
 * 策略 (v2):
 *  - 每 heartbeat_interval_s 发送一次 "ZAIMA\n"
 *  - 超过 heartbeat_timeout_s 未收到响应 -> 判定超时
 *  - 连续超时达阈值 (默认 2 次) -> 判定宕机, 触发 GPIO 复位
 *  - 指数退避: 重启后等待 30/60/120/240/300s 再继续监控
 *  - 最大重启: 1 小时内超过 CONFIG_WD_MAX_REBOOTS_PER_HOUR 次则停止 (防死循环)
 *  - 开机宽限期: 服务器刚重启的 CONFIG_WD_BOOT_GRACE_PERIOD_S 秒内不计超时
 *
 * 通知策略 (v1.3):
 *  - 每次触发宕机重启 -> 推送一条 "宕机通知"
 *  - 触发强制关机     -> 推送一条 "强制关机通知"
 *  - 看门狗停止       -> 推送一条 "看门狗已停止" 通知;
 *    之后管理员在 Web 端执行 "开机" 操作即视为已修复故障, 自动恢复监控
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
#include "notify.h"

#define TAG "WATCHDOG"

#define DEFAULT_HEARTBEAT_INTERVAL  60   // 秒 (每 60 秒发送一次 ZAIMA)
#define DEFAULT_HEARTBEAT_TIMEOUT   600  // 秒 (10 分钟无响应即判定宕机)
#define MAX_CONSECUTIVE_TIMEOUTS    2    // 连续 2 次心跳超时才判定宕机, 避免偶发 USB 丢包误重启
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

    // === 自动保护 ===
    bool auto_poweroff_enabled;       // 连续多次重启后是否强制关机
    uint32_t auto_poweroff_reboots;   // 连续多少次重启未恢复才算"多次" (Web 端可自定义)
    bool paused;                      // 因"主动软关机"暂停监控 (Web 点开机后自动恢复)
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

/**
 * 内部: 只置"暂停"标记, 让看门狗任务在下一个检查点自行退出。
 *
 * 与 watchdog_pause() 的区别: 不等待、不 vTaskDelete —— 供 USB RX 回调这类
 * 不应长时间阻塞的上下文调用 (收到 BYE 时就是在 RX 任务里)。
 */
static void watchdog_request_pause(void)
{
    s_wd.paused   = true;
    s_wd.running  = false;
    s_wd.state    = WD_STATE_IDLE;
}

// USB 回调: 收到响应
static void usb_packet_handler(const usb_packet_t *packet)
{
    if (!packet) return;
    if (packet->cmd == USB_CMD_ACK || packet->cmd == USB_CMD_PING) {
        watchdog_notify_response();
    } else if (packet->cmd == USB_CMD_BYE) {
        // BYE = 服务器主动关机 / 重启前通知 (守护进程退出前会发)。
        // 之前这里只把状态标成 SERVER_DOWN, 监控照跑: 服务器关机后不再回心跳,
        // 下一轮就被判宕机并 GPIO 复位 —— 刚关掉的机器又被按开机。
        // 正确做法与"Web 软关机"一致: 暂停监控, 等管理员在 Web 点"开机"再恢复。
        ESP_LOGW(TAG, "Server sent BYE (graceful shutdown), pausing watchdog");
        LOG_W("服务器发送 BYE (主动关机), 已暂停监控; Web 端执行\"开机\"后自动恢复");
        watchdog_request_pause();
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

        // 检查 USB 是否已连接 (真实判定: 还能收到主机 SOF 包)
        if (!usb_is_connected()) {
            if (s_wd.state != WD_STATE_IDLE) {
                ESP_LOGW(TAG, "USB host disconnected (no SOF), monitoring suspended");
                LOG_W("USB 主机已断开, 暂停监控 (接回后自动恢复)");
                s_wd.state = WD_STATE_IDLE;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 开机宽限期: 服务器刚重启, 暂不判定超时
        // (用差值比较, uint32 毫秒计数约 49.7 天回绕时依然正确)
        int32_t grace_left_ms = (s_wd.boot_grace_until_ms != 0)
                                ? (int32_t)(s_wd.boot_grace_until_ms - now_ms) : 0;
        if (grace_left_ms > 0) {
            // 宽限期内持续刷新 last_response, 避免宽限期结束瞬间立即超时
            s_wd.last_response_ms = now_ms;
            s_wd.consecutive_timeouts = 0;
        } else if (s_wd.boot_grace_until_ms != 0) {
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

                    // 每小时重启上限: 默认 CONFIG_WD_MAX_REBOOTS_PER_HOUR。
                    // 但若用户把"连续 N 次强制关机"调得更高, 这个上限必须同步抬高 ——
                    // 否则重启次数还没到 N 就被小时上限拦下, 自定义阈值永远触发不了。
                    uint32_t hour_limit = (s_wd.auto_poweroff_reboots > (uint32_t)CONFIG_WD_MAX_REBOOTS_PER_HOUR)
                                          ? s_wd.auto_poweroff_reboots
                                          : (uint32_t)CONFIG_WD_MAX_REBOOTS_PER_HOUR;

                    if (s_wd.reboot_count_in_hour > hour_limit) {
                        ESP_LOGE(TAG, "MAX REBOOTS (%lu/hour) REACHED - stopping watchdog",
                                 (unsigned long)hour_limit);
                        LOG_F("看门狗已停止: 1 小时内重启次数过多 (%lu 次), 需管理员介入",
                              (unsigned long)hour_limit);
                        notify_send_event(NOTIFY_EVENT_WATCHDOG_STOPPED, "看门狗已停止",
                                          "1 小时内重启次数过多, 已停止自动重启, 需管理员介入");
                        gpio_set_led_state(LED_BLINK_FAST);  // 快闪 = 需管理员介入
                        s_wd.running = false;
                        break;
                    }

                    // 连续多次重启后服务器仍未恢复 -> 触发强制关机 (可禁用、次数可配置), 并推送通知
                    if (s_wd.auto_poweroff_enabled &&
                        s_wd.consecutive_reboots >= s_wd.auto_poweroff_reboots) {
                        ESP_LOGE(TAG, "Consecutive reboots reached %lu - forcing server power off",
                                 (unsigned long)s_wd.auto_poweroff_reboots);
                        LOG_F("连续 %lu 次重启后服务器仍未恢复 -> 触发强制关机 (需管理员开机)",
                              (unsigned long)s_wd.auto_poweroff_reboots);

                        char alert[160];
                        snprintf(alert, sizeof(alert),
                                 "服务器连续 %lu 次重启后仍未恢复, 看门狗已执行强制关机, 请管理员检查后重新开机",
                                 (unsigned long)s_wd.auto_poweroff_reboots);
                        notify_send_event(NOTIFY_EVENT_FORCE_POWEROFF, "强制关机通知", alert);

                        // 强制关机 = 长按电源键 5 秒
                        gpio_force_poweroff();
                        gpio_set_led_state(LED_BLINK_FAST);   // 快闪 = 需管理员介入

                        // 服务器已断电, 继续监控没有意义: 停止任务,
                        // 管理员开机 (Web"开机"按钮) 时会调用 watchdog_resume() 恢复
                        s_wd.state = WD_STATE_IDLE;
                        s_wd.running = false;
                        break;
                    }

                    // 通知策略: 每次触发宕机重启推送一条 "宕机通知"
                    // (强制关机分支在上方已 break, 走到这里的一定是 GPIO 复位重启)
                    {
                        char alert[160];
                        snprintf(alert, sizeof(alert),
                                 "心跳超时 %lu 秒无响应, 已触发服务器重启 (连续第 %lu 次)",
                                 (unsigned long)elapsed_s, (unsigned long)s_wd.consecutive_reboots);
                        notify_send_event(NOTIFY_EVENT_SERVER_DOWN, "宕机通知", alert);
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
    s_wd.auto_poweroff_enabled = true;   // 默认开启, 由 NVS 配置覆盖
    s_wd.auto_poweroff_reboots = WD_AUTO_POWEROFF_AFTER_REBOOTS;  // 默认次数, 由 NVS 配置覆盖
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
    s_wd.paused  = false;   // 只要重新拉起监控, 就不再是"暂停"状态
    BaseType_t ret = xTaskCreatePinnedToCore(
        watchdog_task, "watchdog_task", 4096, NULL, 3, &s_wd.task_handle, 0);
    if (ret != pdPASS) {
        s_wd.running = false;
        ESP_LOGE(TAG, "Failed to create watchdog task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * 释放两路输出引脚 (复位 / 开机键)。
 * 仅用于"任务被强制删除"前兜底: 若任务正卡在 gpio_force_poweroff() 长按电源键
 * 或 gpio_trigger_server_reset() 的中间被删掉, GPIO 会停在有效电平 ——
 * 表现为电源键或复位线被一直按住, 服务器再也起不来。
 */
static void gpio_release_outputs(void)
{
    gpio_set_level(CONFIG_RESET_GPIO_PIN,    !CONFIG_RESET_ACTIVE_LEVEL);
    gpio_set_level(CONFIG_POWERON_GPIO_PIN,  !CONFIG_POWERON_ACTIVE_LEVEL);
}

void watchdog_stop(void)
{
    s_wd.running = false;
    s_wd.paused  = false;   // 主动停止 ≠ 软关机暂停: Web 端显示为红色告警

    if (!s_wd.task_handle) return;

    // 先给任务 3 秒自行退出 (心跳循环与退避等待循环都会检查 running),
    // 实在退不出再强制删除 —— 直接 vTaskDelete 有把 GPIO 留在有效电平的风险。
    for (int i = 0; i < 30 && s_wd.task_handle; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_wd.task_handle) {
        ESP_LOGW(TAG, "Watchdog task did not exit in time, force delete (GPIO released first)");
        gpio_release_outputs();
        vTaskDelete(s_wd.task_handle);
        s_wd.task_handle = NULL;
    }
}

/**
 * 主动软关机后暂停监控。
 *
 * 背景: "关机（脉冲）"是短按电源键让系统正常关机, 但看门狗无法区分
 *       "主动关机" 与 "宕机" —— 服务器停止回复心跳后会被判定宕机并
 *       再次触发 GPIO 复位, 把刚关掉的机器又按开机。
 *
 * 做法: 与"强制关机/重启过多"的停止不同, 这里置 paused 标记,
 *       让 Web 端显示为"监控已暂停 (服务器已关机)"而不是红色告警,
 *       并在下次点"开机"时由 watchdog_resume() 自动恢复。
 */
void watchdog_pause(void)
{
    // 置标记让任务自行退出; 任务已停时这里等价于"仅标记"
    watchdog_request_pause();

    // 等任务在下一个检查点自行退出, 避免 vTaskDelete 与任务末尾自删除竞争
    for (int i = 0; i < 25 && s_wd.task_handle; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_wd.task_handle) {
        // 兜底: 同样先把 GPIO 拉回无效电平, 再强制删除
        gpio_release_outputs();
        vTaskDelete(s_wd.task_handle);
        s_wd.task_handle = NULL;
    }

    ESP_LOGW(TAG, "Watchdog paused (graceful shutdown). Will resume on power on.");
    LOG_W("监控已暂停 (服务器已关机), Web 端执行\"开机\"后自动恢复");
    gpio_set_led_state(LED_BLINK_SLOW);   // 慢闪 = 已暂停, 与"快闪=需管理员介入"区分
}

bool watchdog_is_paused(void)
{
    return s_wd.paused;
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
    stats->consecutive_reboots  = s_wd.consecutive_reboots;
    stats->running              = s_wd.running;
    stats->auto_poweroff_enabled = s_wd.auto_poweroff_enabled;
    stats->paused               = s_wd.paused;
}

void watchdog_reset_stats(void)
{
    // 时间戳必须刷新为当前时刻而非清 0:
    // 若清 0, 下一周期 elapsed = now - 0 = 开机秒数, 会立刻误判超时,
    // 连续两次就触发 GPIO 复位 —— Web 端"清零"按钮会把正常的服务器硬复位。
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_wd.heartbeat_count      = 0;
    s_wd.response_count       = 0;
    s_wd.timeout_count        = 0;
    s_wd.consecutive_timeouts = 0;
    s_wd.last_heartbeat_ms    = now_ms;
    s_wd.last_response_ms     = now_ms;

    // 自动保护计数也一并清零: Web 端"清零"按钮所在的心跳统计卡片里就显示着
    // "连续重启 (自动保护计数)", 点了却不清会让人以为按钮坏了。
    // (如不希望清零保护计数, 把下面三行去掉即可)
    s_wd.consecutive_reboots  = 0;
    s_wd.reboot_count_in_hour = 0;
    s_wd.window_start_ms      = now_ms;
    s_wd.stable_since_ms      = 0;

    ESP_LOGI(TAG, "Watchdog stats reset");
    LOG_I("心跳统计与自动保护计数已清零");
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

void watchdog_set_auto_poweroff(bool enabled)
{
    s_wd.auto_poweroff_enabled = enabled;
    ESP_LOGI(TAG, "Auto poweroff after %lu consecutive reboots: %s",
             (unsigned long)s_wd.auto_poweroff_reboots, enabled ? "enabled" : "disabled");
}

bool watchdog_get_auto_poweroff(void)
{
    return s_wd.auto_poweroff_enabled;
}

void watchdog_set_auto_poweroff_count(uint32_t count)
{
    // 防御: watchdog_init() 之前被调用时阈值为 0, 这里兜底回默认值,
    // 否则 "consecutive_reboots >= 0" 恒成立, 一宕机就立刻强制关机。
    if (count == 0) {
        count = WD_AUTO_POWEROFF_AFTER_REBOOTS;
    }
    if (count < WD_AUTO_POWEROFF_MIN_REBOOTS) count = WD_AUTO_POWEROFF_MIN_REBOOTS;
    if (count > WD_AUTO_POWEROFF_MAX_REBOOTS) count = WD_AUTO_POWEROFF_MAX_REBOOTS;

    s_wd.auto_poweroff_reboots = count;
    ESP_LOGI(TAG, "Auto poweroff threshold: %lu consecutive reboots", (unsigned long)count);
}

uint32_t watchdog_get_auto_poweroff_count(void)
{
    return (s_wd.auto_poweroff_reboots == 0) ? (uint32_t)WD_AUTO_POWEROFF_AFTER_REBOOTS
                                             : s_wd.auto_poweroff_reboots;
}

esp_err_t watchdog_resume(void)
{
    // 管理员介入 (Web 点"开机") 后调用: 视为故障已修复,
    // 清零退避与连续重启计数, 重新开始监控
    s_wd.consecutive_reboots    = 0;
    s_wd.reboot_count_in_hour   = 0;
    s_wd.window_start_ms        = (uint32_t)(esp_timer_get_time() / 1000);
    s_wd.stable_since_ms        = 0;
    s_wd.consecutive_timeouts   = 0;
    s_wd.paused                 = false;   // 软关机暂停 -> 开机后恢复监控
    s_wd.boot_grace_until_ms    = (uint32_t)(esp_timer_get_time() / 1000)
                                  + (CONFIG_WD_BOOT_GRACE_PERIOD_S * 1000);
    // 停止前可能停留在 SERVER_DOWN, 恢复时复位为正常状态
    if (s_wd.state == WD_STATE_SERVER_DOWN) {
        s_wd.state = WD_STATE_HEALTHY;
        LOG_I("服务器连接已恢复");
    }
    gpio_set_led_state(LED_OFF);

    if (!s_wd.running) {
        ESP_LOGW(TAG, "Watchdog was stopped, restarting monitoring");
        LOG_I("看门狗已恢复监控 (连续重启计数已清零)");
        return watchdog_start(0, 0);
    }

    ESP_LOGI(TAG, "Watchdog backoff counters reset");
    LOG_I("看门狗连续重启计数已清零");
    return ESP_OK;
}
