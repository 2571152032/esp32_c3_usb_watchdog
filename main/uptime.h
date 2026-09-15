/**
 * @file uptime.h
 * @brief 系统运行时间 + 服务器重启计数
 */

#ifndef UPTIME_H
#define UPTIME_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 uptime (记录启动时刻, 加载累计重启次数)
 */
esp_err_t uptime_init(void);

/**
 * @brief 当前运行秒数
 */
uint32_t uptime_get_seconds(void);

/**
 * @brief 格式化 "Xd HH:MM:SS" 或 "HH:MM:SS"
 */
const char *uptime_format(char *buf, size_t buf_len);

/**
 * @brief 服务器被重启次数 +1 (由 watchdog 调用)
 */
void uptime_inc_reboots(void);

/**
 * @brief 获取累计服务器重启次数
 */
uint32_t uptime_get_reboots(void);

#ifdef __cplusplus
}
#endif

#endif // UPTIME_H
