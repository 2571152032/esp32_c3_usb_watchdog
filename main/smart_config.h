/**
 * @file smart_config.h
 * @brief SmartConfig 智能配网 (AP 模式 + Web 向导)
 *
 * 首次启动或长按复位键后:
 * 1. ESP32-C3 创建 AP 热点
 * 2. 手机连接热点，浏览器打开 192.168.4.1
 * 3. Web 页面扫描可用 WiFi，选择并输入密码
 * 4. 保存配置，重启连接 WiFi
 */

#ifndef SMART_CONFIG_H
#define SMART_CONFIG_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 配网结果回调
typedef void (*smart_config_callback_t)(esp_err_t result, const char *ssid);

/**
 * @brief 启动智能配网 (AP 模式 + Web 服务器)
 * @param callback 配网完成回调 (可选, NULL 则不回调)
 * @return esp_err_t
 *
 * 此函数会阻塞直到:
 * - 配网成功 (返回 ESP_OK)
 * - 超时 (默认 5 分钟, 返回 ESP_ERR_TIMEOUT)
 * - 用户取消 (返回 ESP_FAIL)
 */
esp_err_t smart_config_start(void);

/**
 * @brief 停止智能配网
 */
void smart_config_stop(void);

/**
 * @brief 获取当前配网状态
 */
typedef enum {
    SC_STATE_IDLE,
    SC_STATE_AP_STARTED,
    SC_STATE_WEB_SERVER_STARTED,
    SC_STATE_SCANNING,
    SC_STATE_CONNECTING,
    SC_STATE_SUCCESS,
    SC_STATE_FAILED,
    SC_STATE_TIMEOUT,
} smart_config_state_t;

smart_config_state_t smart_config_get_state(void);

#ifdef __cplusplus
}
#endif

#endif // SMART_CONFIG_H
