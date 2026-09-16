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

#ifdef __cplusplus
}
#endif

#endif // WATCHDOG_H
