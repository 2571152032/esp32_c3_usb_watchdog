/**
 * @file nvs_storage.c
 * @brief NVS 配置存储实现
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "nvs_storage.h"

#define TAG "NVS"

static nvs_handle_t s_nvs_handle = 0;
static bool s_initialized = false;

esp_err_t nvs_storage_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "NVS storage initialized");
    return ESP_OK;
}

esp_err_t nvs_save_wifi_config(const char *ssid, const char *password)
{
    if (!s_initialized) return ESP_FAIL;
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;

    esp_err_t ret;

    ret = nvs_set_str(s_nvs_handle, NVS_KEY_SSID, ssid);
    if (ret != ESP_OK) goto err;

    ret = nvs_set_str(s_nvs_handle, NVS_KEY_PASSWORD, password);
    if (ret != ESP_OK) goto err;

    ret = nvs_set_u8(s_nvs_handle, NVS_KEY_CONFIGURED, 1);
    if (ret != ESP_OK) goto err;

    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) goto err;

    ESP_LOGI(TAG, "WiFi config saved: SSID=%s", ssid);
    return ESP_OK;

err:
    ESP_LOGE(TAG, "Failed to save WiFi config: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t nvs_get_wifi_ssid(char *ssid, size_t max_len)
{
    if (!s_initialized) return ESP_FAIL;

    size_t len = max_len;
    esp_err_t ret = nvs_get_str(s_nvs_handle, NVS_KEY_SSID, ssid, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read SSID: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t nvs_get_wifi_password(char *password, size_t max_len)
{
    if (!s_initialized) return ESP_FAIL;

    size_t len = max_len;
    esp_err_t ret = nvs_get_str(s_nvs_handle, NVS_KEY_PASSWORD, password, &len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read password: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t nvs_clear_wifi_config(void)
{
    if (!s_initialized) return ESP_FAIL;

    ESP_LOGI(TAG, "Clearing WiFi configuration...");

    nvs_erase_key(s_nvs_handle, NVS_KEY_SSID);
    nvs_erase_key(s_nvs_handle, NVS_KEY_PASSWORD);
    nvs_erase_key(s_nvs_handle, NVS_KEY_CONFIGURED);
    nvs_commit(s_nvs_handle);

    // 重置网络后, 一并恢复默认 Web 认证凭据
    nvs_restore_default_credentials();

    ESP_LOGI(TAG, "WiFi config cleared, credentials restored to default");
    return ESP_OK;
}

bool nvs_is_configured(void)
{
    if (!s_initialized) return false;

    uint8_t configured = 0;
    esp_err_t ret = nvs_get_u8(s_nvs_handle, NVS_KEY_CONFIGURED, &configured);
    return (ret == ESP_OK && configured == 1);
}

esp_err_t nvs_save_heartbeat_params(uint32_t interval, uint32_t timeout)
{
    if (!s_initialized) return ESP_FAIL;

    esp_err_t ret;
    ret = nvs_set_u32(s_nvs_handle, NVS_KEY_HB_INTERVAL, interval);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u32(s_nvs_handle, NVS_KEY_HB_TIMEOUT, timeout);
    if (ret != ESP_OK) return ret;
    return nvs_commit(s_nvs_handle);
}

esp_err_t nvs_load_heartbeat_params(uint32_t *interval, uint32_t *timeout)
{
    if (!s_initialized) return ESP_FAIL;

    esp_err_t ret;
    if (interval) {
        ret = nvs_get_u32(s_nvs_handle, NVS_KEY_HB_INTERVAL, interval);
        if (ret != ESP_OK) *interval = 60;  // 默认 60 秒
    }
    if (timeout) {
        ret = nvs_get_u32(s_nvs_handle, NVS_KEY_HB_TIMEOUT, timeout);
        if (ret != ESP_OK) *timeout = 600;  // 默认 10 分钟
    }
    return ESP_OK;
}

// ==================== Web 认证凭据 ====================

esp_err_t nvs_restore_default_credentials(void)
{
    if (!s_initialized) return ESP_FAIL;

    esp_err_t ret;
    ret = nvs_set_str(s_nvs_handle, NVS_KEY_WEB_USER, DEFAULT_WEB_USERNAME);
    if (ret != ESP_OK) goto err;
    ret = nvs_set_str(s_nvs_handle, NVS_KEY_WEB_PASS, DEFAULT_WEB_PASSWORD);
    if (ret != ESP_OK) goto err;
    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) goto err;

    ESP_LOGI(TAG, "Web credentials restored to default: %s / %s",
             DEFAULT_WEB_USERNAME, DEFAULT_WEB_PASSWORD);
    return ESP_OK;

err:
    ESP_LOGE(TAG, "Failed to restore default credentials: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t nvs_get_credentials(char *user, size_t user_len, char *pass, size_t pass_len)
{
    if (!s_initialized) return ESP_FAIL;
    if (!user || !pass) return ESP_ERR_INVALID_ARG;

    size_t len;
    esp_err_t ret;

    len = user_len;
    ret = nvs_get_str(s_nvs_handle, NVS_KEY_WEB_USER, user, &len);
    if (ret != ESP_OK) {
        // 首次启动无凭据, 写入默认值
        strcpy(user, DEFAULT_WEB_USERNAME);
        strcpy(pass, DEFAULT_WEB_PASSWORD);
        nvs_restore_default_credentials();
        return ESP_OK;
    }

    len = pass_len;
    ret = nvs_get_str(s_nvs_handle, NVS_KEY_WEB_PASS, pass, &len);
    if (ret != ESP_OK) {
        strcpy(pass, DEFAULT_WEB_PASSWORD);
    }
    return ESP_OK;
}

esp_err_t nvs_save_credentials(const char *user, const char *pass)
{
    if (!s_initialized) return ESP_FAIL;
    if (!user || !pass) return ESP_ERR_INVALID_ARG;
    if (strlen(user) == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t ret;
    ret = nvs_set_str(s_nvs_handle, NVS_KEY_WEB_USER, user);
    if (ret != ESP_OK) goto err;
    ret = nvs_set_str(s_nvs_handle, NVS_KEY_WEB_PASS, pass);
    if (ret != ESP_OK) goto err;
    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) goto err;

    ESP_LOGI(TAG, "Web credentials updated: user=%s", user);
    return ESP_OK;

err:
    ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(ret));
    return ret;
}
