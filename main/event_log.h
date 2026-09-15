/**
 * @file event_log.h
 * @brief 事件日志模块 (RAM 环形缓冲 + NVS 持久化关键事件)
 *
 * 设计:
 *  - RAM 环形缓冲: 256 条, 实时展示, 重启后清空
 *  - NVS 持久化: WARN 及以上级别自动落盘, 32 条, 重启后保留
 *  - 通过 Web /api/logs 读取
 */

#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_MAX_RAM_ENTRIES     256
#define LOG_MAX_NVS_ENTRIES     32
#define LOG_MAX_MESSAGE_LEN     128

typedef enum {
    LOG_LEVEL_INFO = 0,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL,
} log_level_t;

typedef struct {
    uint32_t    seq;                // 序号
    uint32_t    timestamp_ms;       // 系统启动后毫秒数
    log_level_t level;
    char        message[LOG_MAX_MESSAGE_LEN];
} log_entry_t;

/**
 * @brief 初始化日志模块
 */
esp_err_t event_log_init(void);

/**
 * @brief 写入一条日志 (支持 printf 格式)
 */
void event_log_write(log_level_t level, const char *fmt, ...);

// 便捷宏
#define LOG_I(fmt, ...)    event_log_write(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...)    event_log_write(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...)    event_log_write(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_F(fmt, ...)    event_log_write(LOG_LEVEL_FATAL, fmt, ##__VA_ARGS__)

/**
 * @brief 读取 RAM 日志 (从最旧到最新)
 * @param entries   输出缓冲
 * @param max_count 最大读取条数
 * @return          实际读取条数
 */
uint32_t event_log_read(log_entry_t *entries, uint32_t max_count);

/**
 * @brief 读取 NVS 持久化日志 (从最旧到最新)
 */
uint32_t event_log_read_nvs(log_entry_t *entries, uint32_t max_count);

uint32_t event_log_get_ram_count(void);
uint32_t event_log_get_nvs_count(void);

void     event_log_clear_ram(void);
esp_err_t event_log_clear_nvs(void);

/**
 * @brief 将启动后毫秒数格式化为 "HH:MM:SS"
 */
const char *event_log_format_time(uint32_t timestamp_ms, char *buf, size_t buf_len);

/**
 * @brief 日志级别字符串
 */
const char *event_log_level_str(log_level_t level);

#ifdef __cplusplus
}
#endif

#endif // EVENT_LOG_H
