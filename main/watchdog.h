/**
 * @file watchdog.h
 * @brief USB 看门狗检测逻辑接口
 */

#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 看门狗状态
typedef enum {
    WD_STATE_IDLE,           // 空闲 (USB 未连接)
    WD_STATE_WAITING,        // 等待响应
    WD_STATE_HEALTHY,        // 服务器正常
    WD_STATE_WARNING,        // 接近超时
    WD_STATE_SERVER_DOWN,    // 服务器宕机
} watchdog_state_t;

// ==================== 状态趋势 (供 Web SVG 图表) ====================
#define WD_TREND_MAX_POINTS  60

typedef enum {
    TREND_OK = 0,       // 周期内收到响应 (健康)
    TREND_TIMEOUT,      // 周期内超时
    TREND_DOWN,         // 判定宕机
} trend_point_t;

uint32_t watchdog_get_trend(trend_point_t *points, uint32_t max);
uint32_t watchdog_get_trend_count(void);

// ==================== 策略参数 ====================
#ifndef CONFIG_WD_MAX_REBOOTS_PER_HOUR
#define CONFIG_WD_MAX_REBOOTS_PER_HOUR  5    // 1 小时内最多重启次数 (超过则停止)
#endif
#ifndef CONFIG_WD_BOOT_GRACE_PERIOD_S
#define CONFIG_WD_BOOT_GRACE_PERIOD_S   180  // 服务器开机后宽限期 (秒, 期间不计超时)
#endif
#ifndef WD_AUTO_POWEROFF_AFTER_REBOOTS
#define WD_AUTO_POWEROFF_AFTER_REBOOTS  3    // 默认: 连续重启达到该次数后触发强制关机
#endif
// "连续多少次重启后强制关机" 的允许范围 (可在 Web 端自定义)
#define WD_AUTO_POWEROFF_MIN_REBOOTS    1
#define WD_AUTO_POWEROFF_MAX_REBOOTS    20

// 指数退避: 重启后等待时间 (秒): 30/60/120/240/300
uint32_t watchdog_get_retry_delay(void);

// 看门狗统计信息
typedef struct {
    watchdog_state_t state;
    uint32_t last_heartbeat_time;   // 最后一次发送心跳的时间 (ms)
    uint32_t last_response_time;    // 最后一次收到响应的时间 (ms)
    uint32_t heartbeat_count;       // 已发送心跳计数
    uint32_t response_count;        // 已收到响应计数
    uint32_t timeout_count;         // 超时计数
    uint32_t consecutive_timeouts;  // 连续超时次数
    uint32_t consecutive_reboots;   // 连续重启次数 (稳定运行 5 分钟后自动清零)
    bool     running;               // 监控任务是否在运行
    bool     auto_poweroff_enabled; // 连续多次重启后是否强制关机
    bool     paused;                // 因"主动软关机"暂停监控 (点开机即恢复)
    bool     pause_on_usb_lost;     // 检测不到 USB 主机时是否暂停监控 (见 watchdog_set_pause_on_usb_lost)
} watchdog_stats_t;

/**
 * @brief 初始化看门狗 (清零状态、设置默认值)
 */
esp_err_t watchdog_init(void);

/**
 * @brief 启动看门狗监控 (带参数版本)
 * @param heartbeat_interval_ms 心跳间隔 (毫秒)
 * @param timeout_ms 超时时间 (毫秒)
 * @note  传入 (0, 0) 时使用当前已设置的参数 (watchdog_set_params / 默认值)
 */
esp_err_t watchdog_start(uint32_t heartbeat_interval_ms, uint32_t timeout_ms);

/**
 * @brief 停止看门狗监控
 */
void watchdog_stop(void);

/**
 * @brief 暂停监控 (主动软关机后调用)
 *
 * 与 watchdog_stop() 的区别: 会置 paused 标记, Web 端显示为
 * "监控已暂停 (服务器已关机)" 而非红色告警; 点"开机"时由
 * watchdog_resume() 自动恢复。
 * 目的: 软关机后服务器不再回复心跳, 若不暂停会被误判宕机并重新开机。
 */
void watchdog_pause(void);

/**
 * @brief 是否处于"软关机暂停"状态
 */
bool watchdog_is_paused(void);

/**
 * @brief 重置看门狗运行时状态 (服务器恢复后调用)
 */
void watchdog_reset_state(void);

/**
 * @brief 兼容旧名
 */
void watchdog_reset(void);

/**
 * @brief 检查服务器是否宕机
 */
bool watchdog_is_server_down(void);

/**
 * @brief 获取当前状态
 */
watchdog_state_t watchdog_get_state(void);

/**
 * @brief 获取统计信息
 */
void watchdog_get_stats(watchdog_stats_t *stats);

/**
 * @brief 清零运行时统计 (心跳数 / 响应数 / 超时数 / 连续超时), 不影响参数与状态
 */
void watchdog_reset_stats(void);

/**
 * @brief 通知收到心跳响应 (由 USB 回调调用)
 */
void watchdog_notify_response(void);

/**
 * @brief 设置心跳参数 (秒)
 */
void watchdog_set_params(uint32_t interval_s, uint32_t timeout_s);

/**
 * @brief 获取当前心跳参数 (秒)
 */
void watchdog_get_params(uint32_t *interval_s, uint32_t *timeout_s);

/**
 * @brief 注册服务器宕机回调 (GPIO 复位在此触发)
 */
void watchdog_register_server_down_callback(void (*cb)(void));

/**
 * @brief 启用 / 禁用 "连续多次重启后强制关机"
 */
void watchdog_set_auto_poweroff(bool enabled);

/**
 * @brief 查询 "连续多次重启后强制关机" 是否启用
 */
bool watchdog_get_auto_poweroff(void);

/**
 * @brief 设置 "连续 N 次重启未恢复 -> 自动强制关机" 的次数阈值
 * @param count 次数, 超出 [WD_AUTO_POWEROFF_MIN_REBOOTS, WD_AUTO_POWEROFF_MAX_REBOOTS] 会被钳制
 */
void watchdog_set_auto_poweroff_count(uint32_t count);

/**
 * @brief 获取当前 "连续 N 次重启 -> 强制关机" 的次数阈值
 */
uint32_t watchdog_get_auto_poweroff_count(void);

/**
 * @brief 恢复监控 (强制关机后由人工开机时调用)
 *        清零连续重启计数与退避, 并在任务已停止时重新拉起
 */
esp_err_t watchdog_resume(void);

/**
 * @brief 设置 "检测不到 USB 主机时是否暂停监控"
 *
 * 两种行为各有取舍, 由用户按实际场景选择 (Web「看门狗参数」卡片可切换):
 *
 *  - true  (暂停): USB 断开(收不到主机 SOF)时只置 IDLE, 不发心跳也不判宕机。
 *          好处: 拔掉 USB 线维护时不会周期性地误复位正常运行的服务器。
 *          代价: 若服务器硬挂到连 USB 主机控制器都停了, 看门狗也不复位。
 *
 *  - false (继续监控, 默认): 不管 USB 是否断开都照常发心跳、照常判超时并复位。
 *          好处: 不漏掉"死机死到 USB 一起失效"的场景。
 *          代价: USB 线拔着的时候, 正常运行的服务器会被周期性复位。
 *
 * @note 仅影响"是否继续监控", 不影响 Web 上 USB 连接状态的显示 ——
 *       那个始终是 usb_serial_jtag_is_connected() 的真实值。
 */
void watchdog_set_pause_on_usb_lost(bool enabled);

/**
 * @brief 查询 "USB 断开时是否暂停监控" (默认 false = 继续监控)
 */
bool watchdog_get_pause_on_usb_lost(void);

/**
 * @brief 设置 "USB 串口断开时是否软重启看门狗设备本身"
 *
 * 默认 false (不重启)。开启后一旦检测到 USB 主机断开 (收不到 SOF 包),
 * 看门狗设备立即软重启 (esp_restart), 与网页「设备重启」行为相同。
 *
 * @note 若 USB 长期保持断开, 设备会反复重启 (每次启动都会再次检测到断开);
 *       适用于"USB 断开即代表需要复位看门狗"的部署场景。
 */
void watchdog_set_reboot_on_usb_lost(bool enabled);

/**
 * @brief 查询 "USB 串口断开时是否软重启看门狗设备" (默认 false = 不重启)
 */
bool watchdog_get_reboot_on_usb_lost(void);

#ifdef __cplusplus
}
#endif

#endif // WATCHDOG_H
