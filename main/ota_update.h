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

// ==================== 在线检查新固件 ====================
//
// 【要改地址就改下面这一行 OTA_CHECK_URL 的值】
//
// 设备对 OTA_CHECK_URL 发一个 GET，按返回内容判断是否已有新版本。
// 服务端返回内容支持两种写法（自动识别，不用额外配置）:
//
//   1) JSON（推荐）:
//        {"version":"1.3.3","url":"https://你的域名/esp32c3_watchdog.bin"}
//      - version: 最新版本号 (必填)，与镜像里的 PROJECT_VER 比较
//      - url    : 固件下载地址 (可省略；省略时 Web 端只提示版本，不给下载链接)
//      例: {"version":"1.3.3","url":"https://example.com/fw/esp32_c3_usb_watchdog.bin"}
//
//   2) 纯文本: 直接返回版本号即可，例如文件里就一行  1.3.3
//
// 留空字符串 "" 表示禁用在线检查（Web 端会提示"未配置在线检查地址"）。
#ifndef OTA_CHECK_URL
#define OTA_CHECK_URL  ""
#endif

// 在线检查状态
typedef enum {
    OTA_CHECK_IDLE = 0,   // 未检查过
    OTA_CHECK_RUNNING,    // 正在检查
    OTA_CHECK_OK,         // 检查完成
    OTA_CHECK_FAILED,     // 检查失败
} ota_check_state_t;

// 在线检查结果
typedef struct {
    ota_check_state_t state;
    bool  has_update;
    char  latest_version[32];   // 线上最新版本号
    char  url[192];             // 线上固件下载地址 (可能为空)
    char  message[96];          // 可直接展示给用户的中文提示
} ota_check_info_t;

/**
 * @brief 获取最近一次在线检查的结果 (供 Web 端展示)
 */
void ota_check_get_info(ota_check_info_t *info);

/**
 * @brief 发起一次在线检查 (内部创建临时任务, 不阻塞调用者)
 * @return ESP_OK 已开始; 其它: 未配置地址 / 正在检查 / 内存不足
 */
esp_err_t ota_check_start(void);

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
