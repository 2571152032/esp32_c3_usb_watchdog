/**
 * @file uptime.c
 * @brief 系统运行时间 + 服务器重启计数实现
 *
 * 重启次数持久化到 NVS (key: srv_reboots), 跨重启累计,
 * 用于 "这台服务器总共被看门狗重启了多少次" 统计.
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "uptime.h"
#include "nvs_storage.h"
#include "event_log.h"

#define TAG "UPTIME"

static struct {
    bool     initialized;
    int64_t  boot_time_us;     // 本次启动时刻 (us, 64 位避免 49.7 天回绕)
    uint32_t total_reboots;    // 累计服务器重启次数 (从 NVS 加载)
} s_up = {0};

static nvs_handle_t s_nvs = 0;
static void nvs_ensure(void)
{
    if (s_nvs) return;
    nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
}

esp_err_t uptime_init(void)
{
    if (s_up.initialized) return ESP_OK;

    s_up.boot_time_us = esp_timer_get_time();
    s_up.total_reboots = 0;

    nvs_ensure();
    if (s_nvs) {
        nvs_get_u32(s_nvs, "srv_reboots", &s_up.total_reboots);
    }

    s_up.initialized = true;
    ESP_LOGI(TAG, "Uptime init, total server reboots=%lu",
             (unsigned long)s_up.total_reboots);
    return ESP_OK;
}

uint32_t uptime_get_seconds(void)
{
    if (!s_up.initialized) return 0;
    return (uint32_t)((esp_timer_get_time() - s_up.boot_time_us) / 1000000LL);
}

const char *uptime_format(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 24) return "";

    uint32_t total = uptime_get_seconds();
    uint32_t days  = total / 86400;
    uint32_t hours = (total % 86400) / 3600;
    uint32_t mins  = (total % 3600)  / 60;
    uint32_t secs  = total % 60;

    if (days > 0) {
        snprintf(buf, buf_len, "%lud %02lu:%02lu:%02lu",
                 (unsigned long)days, (unsigned long)hours,
                 (unsigned long)mins, (unsigned long)secs);
    } else {
        snprintf(buf, buf_len, "%02lu:%02lu:%02lu",
                 (unsigned long)hours, (unsigned long)mins, (unsigned long)secs);
    }
    return buf;
}

void uptime_inc_reboots(void)
{
    if (!s_up.initialized) return;
    s_up.total_reboots++;

    nvs_ensure();
    if (s_nvs) {
        nvs_set_u32(s_nvs, "srv_reboots", s_up.total_reboots);
        nvs_commit(s_nvs);
    }
    ESP_LOGI(TAG, "Server reboot count -> %lu", (unsigned long)s_up.total_reboots);
}

uint32_t uptime_get_reboots(void)
{
    return s_up.total_reboots;
}

void uptime_reset_reboots(void)
{
    if (!s_up.initialized) return;
    s_up.total_reboots = 0;

    nvs_ensure();
    if (s_nvs) {
        nvs_set_u32(s_nvs, "srv_reboots", 0);
        nvs_commit(s_nvs);
    }
    ESP_LOGI(TAG, "Server reboot count reset");
    LOG_I("服务器重启计数已清零");
}
