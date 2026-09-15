/**
 * @file ota_update.h
 * @brief 固件更新 (OTA over HTTP) 接口
 */

#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_RECV_BUF_SIZE  1024   // 单次接收缓冲 (字节)

// OTA 状态
typedef enum {
    OTA_STATE_IDLE = 0,       // 空闲
    OTA_STATE_WRITING,        // 正在写入
    OTA_STATE_SUCCESS,        // 成功 (待重启)
    OTA_STATE_FAILED,         // 失败
} ota_state_t;

// OTA 状态信息
typedef struct {
    ota_state_t state;
    uint32_t received_bytes;
    uint32_t total_bytes;
    uint8_t  progress;   // 0-100
} ota_status_t;

/**
 * @brief 获取当前 OTA 状态
 */
ota_state_t ota_get_state(void);

/**
 * @brief 获取上传进度 (0-100)
 */
uint32_t ota_get_progress(void);

/**
 * @brief 获取详细状态
 */
void ota_get_status(ota_status_t *status);

/**
 * @brief 中止当前 OTA 并更新状态为空闲
 */
void ota_abort(void);

/**
 * @brief 是否已处于 "刷写完成、等待重启" 状态
 *        (web_server 据此延迟重启, 让响应先发回前端)
 */
bool ota_is_reboot_pending(void);

/**
 * @brief 初始化 OTA 模块 (在 app_main 中调用)
 *
 * 检查当前运行分区: 若运行在 OTA 分区 (ota_0/ota_1),
 * 则标记新固件为有效 (否则下次重启会自动回滚到上一个分区).
 */
esp_err_t ota_update_init(void);

/**
 * @brief 处理固件上传 (原始二进制流: Content-Type: application/octet-stream)
 * @param req        HTTP 请求 (body 为完整固件二进制)
 * @param total_size 声明的总大小 (来自 Content-Length), 用于容量预校验; 0 表示按实际写入为准
 * @return ESP_OK 上传完成, 调用方应重启设备
 */
esp_err_t ota_handle_raw_upload(httpd_req_t *req, uint32_t total_size);

/**
 * @brief 处理固件上传 (multipart/form-data 兼容入口)
 */
esp_err_t ota_handle_upload(httpd_req_t *req, const char *field_name);

#ifdef __cplusplus
}
#endif

#endif // OTA_UPDATE_H
