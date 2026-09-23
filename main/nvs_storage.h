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
#define NVS_KEY_AUTO_OFF_N  "auto_off_n"    // 连续多少次重启未恢复后强制关机
#define NVS_KEY_NOTIFY_EN   "ntfy_en"       // 通知推送开关
#define NVS_KEY_NOTIFY_URL  "ntfy_url"      // 通知服务地址 (Base URL, 设备会自动追加 /n)
#define NVS_KEY_NOTIFY_TOK  "ntfy_tok"      // 通知鉴权令牌 (Authorization: Bearer <token>)

// 服务器累计重启次数 (由 uptime.c 维护; 恢复出厂时一并清除)
#define NVS_KEY_SRV_REBOOTS "srv_reboots"

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
 * @brief 保存"连续 N 次重启未恢复后强制关机"的次数阈值
 */
esp_err_t nvs_save_auto_poweroff_count(uint32_t count);

/**
 * @brief 加载该次数阈值 (未设置过时返回 ESP_ERR_NOT_FOUND, *count 保持调用前的值)
 */
esp_err_t nvs_load_auto_poweroff_count(uint32_t *count);

/**
 * @brief 保存通知推送配置
 * @param enabled 是否启用
 * @param url     通知服务地址 (Base URL), NULL 表示保持原值
 * @param token   鉴权令牌 (Bearer), NULL 表示保持原值
 */
esp_err_t nvs_save_notify_config(bool enabled, const char *url, const char *token);

/**
 * @brief 加载通知推送配置 (未设置过时 enabled=false, url/token 为空串)
 */
esp_err_t nvs_load_notify_config(bool *enabled, char *url, size_t url_len,
                                 char *token, size_t token_len);

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
