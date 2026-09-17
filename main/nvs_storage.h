/**
 * @file nvs_storage.h
 * @brief NVS 配置存储 (WiFi 凭据、系统配置)
 */

#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_NAMESPACE       "watchdog"
#define NVS_KEY_SSID        "wifi_ssid"
#define NVS_KEY_PASSWORD    "wifi_pass"
#define NVS_KEY_CONFIGURED  "configured"
#define NVS_KEY_HB_INTERVAL "hb_interval"
#define NVS_KEY_HB_TIMEOUT  "hb_timeout"
#define NVS_KEY_AUTO_OFF    "auto_off"      // 连续多次重启后是否强制关机
#define NVS_KEY_NOTIFY_EN   "ntfy_en"       // 通知推送开关
#define NVS_KEY_NOTIFY_URL  "ntfy_url"      // 通知推送地址 (Webhook URL)

// 默认 Web 认证凭据 (重置网络后恢复为此默认值)
#define DEFAULT_WEB_USERNAME  "admin"
#define DEFAULT_WEB_PASSWORD  "admin123"

// Web 认证凭据存储 key
#define NVS_KEY_WEB_USER     "web_user"
#define NVS_KEY_WEB_PASS     "web_pass"

/**
 * @brief 初始化 NVS
 */
esp_err_t nvs_storage_init(void);

/**
 * @brief 保存 WiFi 配置
 */
esp_err_t nvs_save_wifi_config(const char *ssid, const char *password);

/**
 * @brief 获取保存的 WiFi SSID
 */
esp_err_t nvs_get_wifi_ssid(char *ssid, size_t max_len);

/**
 * @brief 获取保存的 WiFi 密码
 */
esp_err_t nvs_get_wifi_password(char *password, size_t max_len);

/**
 * @brief 清除 WiFi 配置 (进入配网模式)
 */
esp_err_t nvs_clear_wifi_config(void);

/**
 * @brief 恢复出厂设置: 清空 WiFi + 心跳参数 + 自动保护 + 通知 + 凭据,
 *        下次启动后所有用户可调值都会回到内置默认值
 */
esp_err_t nvs_factory_reset(void);

/**
 * @brief 检查是否已配置
 */
bool nvs_is_configured(void);

/**
 * @brief 保存心跳参数
 */
esp_err_t nvs_save_heartbeat_params(uint32_t interval, uint32_t timeout);

/**
 * @brief 加载心跳参数 (默认 60/600 秒)
 */
esp_err_t nvs_load_heartbeat_params(uint32_t *interval, uint32_t *timeout);

// ==================== 自动保护 / 通知 ====================

/**
 * @brief 保存"连续多次重启后强制关机"开关
 */
esp_err_t nvs_save_auto_poweroff(bool enabled);

/**
 * @brief 加载"连续多次重启后强制关机"开关 (未设置过时默认为 true)
 */
esp_err_t nvs_load_auto_poweroff(bool *enabled);

/**
 * @brief 保存通知推送配置
 */
esp_err_t nvs_save_notify_config(bool enabled, const char *url);

/**
 * @brief 加载通知推送配置 (未设置过时 enabled=false, url 为空串)
 */
esp_err_t nvs_load_notify_config(bool *enabled, char *url, size_t url_len);

// ==================== Web 认证凭据 ====================

/**
 * @brief 将 Web 认证凭据恢复为默认值 (admin / admin123)
 *        重置网络后会自动调用, 一般无需手动调用
 */
esp_err_t nvs_restore_default_credentials(void);

/**
 * @brief 读取当前认证凭据; 若 NVS 中无凭据则自动写入默认值
 */
esp_err_t nvs_get_credentials(char *user, size_t user_len, char *pass, size_t pass_len);

/**
 * @brief 保存新凭据 (修改用户名/密码)
 */
esp_err_t nvs_save_credentials(const char *user, const char *pass);

#ifdef __cplusplus
}
#endif

#endif // NVS_STORAGE_H
