/**
 * @file main.c
 * @brief ESP32-C3 USB Watchdog - Main Entry
 *
 * 功能:
 *  - USB CDC-ACM 设备模拟，连接 Linux 服务器
 *  - 心跳检测服务器存活状态
 *  - GPIO 触发服务器硬件重启
 *  - SmartConfig 智能配网 (AP + Web)
 *  - Web 控制端
 *  - 长按复位键重新配网
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "usb_device.h"
#include "watchdog.h"
#include "web_server.h"
#include "smart_config.h"
#include "gpio_control.h"
#include "nvs_storage.h"
#include "event_log.h"
#include "uptime.h"
#include "ota_update.h"
#include "notify.h"
#include "esp_task_wdt.h"

#define TAG "MAIN"

// WiFi 连接事件组
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT        BIT1
static EventGroupHandle_t s_wifi_event_group;

// 系统状态
typedef enum {
    SYS_STATE_BOOT,
    SYS_STATE_CONFIG_MODE,     // AP 配网模式
    SYS_STATE_CONNECTING,      // 正在连接 WiFi
    SYS_STATE_RUNNING,         // 正常运行 (USB 看门狗工作中)
} system_state_t;

static system_state_t g_system_state = SYS_STATE_BOOT;

// Task WDT 注册标志: system_task 是否成功注册到 TWDT。
// 必须先 esp_task_wdt_add() 才能 esp_task_wdt_reset(),
// 否则每 100ms 打一条 "task not found" 错误日志 (刷屏 + 浪费 CPU)。
static bool s_task_wdt_registered = false;

// WiFi 事件处理
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// WiFi 协议栈是否已初始化 (重复调用 esp_wifi_init/esp_netif_create 会失败)
static bool s_wifi_stack_ready = false;

// 初始化并启动 WiFi (STA 模式) —— 只做一次, 之后重试连接不再重复初始化
static esp_err_t wifi_init_sta(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

// 等待 WiFi 连接成功 (断开后由事件回调自动重连, 这里只负责等待)
static bool wifi_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// 检查是否需要进入配网模式
static bool should_enter_config_mode(void)
{
    // 首次启动：NVS 中没有保存的 WiFi 配置
    char ssid[64] = {0};
    if (nvs_get_wifi_ssid(ssid, sizeof(ssid)) != ESP_OK) {
        ESP_LOGI(TAG, "No saved WiFi config, entering config mode");
        return true;
    }
    return false;
}

// 启动服务器监控 (USB 看门狗 + Web 服务器), 幂等: 重复调用只启动一次
static void start_monitoring(void)
{
    static bool wd_started = false;
    static bool web_started = false;

    if (!wd_started) {
        // 启动 USB 看门狗
        if (usb_device_init() == ESP_OK) {
            ESP_LOGI(TAG, "USB device initialized");
            watchdog_start(0, 0);
            ESP_LOGI(TAG, "Watchdog started, monitoring server...");
            wd_started = true;
        } else {
            ESP_LOGE(TAG, "USB device init failed");
            LOG_E("USB 设备初始化失败");
        }
    }

    if (!web_started) {
        esp_err_t web_ret = web_server_start();
        if (web_ret != ESP_OK) {
            ESP_LOGW(TAG, "Web server start returned: %s", esp_err_to_name(web_ret));
        } else {
            ESP_LOGI(TAG, "Web server started");
            web_started = true;
        }
    }
}

// 主任务 - 系统状态机
static void system_task(void *pvParameter)
{
    // 把当前任务注册进 Task WDT (之后才能喂狗)。
    // 注意: 不能只在 app_main 里 add(NULL) —— 那注册的是 main 任务,
    //       而 main 任务在 app_main() 返回后就被删除, 会留下悬空注册项。
    if (esp_task_wdt_add(NULL) == ESP_OK) {
        s_task_wdt_registered = true;
        ESP_LOGI(TAG, "system_task registered to Task WDT");
    } else {
        ESP_LOGW(TAG, "Task WDT unavailable, skip feeding");
    }

    while (1) {
        switch (g_system_state) {
            case SYS_STATE_BOOT: {
                // 检查复位按钮是否长按 (在启动时检测)
                if (gpio_is_button_pressed_long()) {
                    ESP_LOGI(TAG, "Long press detected at boot, clearing config");
                    LOG_W("启动时检测到长按 -> 恢复出厂设置并重启");
                    nvs_factory_reset();
                    esp_restart();
                }

                if (should_enter_config_mode()) {
                    g_system_state = SYS_STATE_CONFIG_MODE;
                } else {
                    g_system_state = SYS_STATE_CONNECTING;
                }
                break;
            }

            case SYS_STATE_CONFIG_MODE: {
                ESP_LOGI(TAG, "Entering config mode (AP)...");
                LOG_I("进入配网 (AP) 模式");
                smart_config_start();
                // smart_config 会阻塞直到配网完成或超时
                // 配网完成后重启进入正常运行
                break;
            }

            case SYS_STATE_CONNECTING: {
                if (!s_wifi_stack_ready) {
                    char ssid[64] = {0};
                    char password[64] = {0};
                    nvs_get_wifi_ssid(ssid, sizeof(ssid));
                    nvs_get_wifi_password(password, sizeof(password));
                    wifi_init_sta(ssid, password);
                    s_wifi_stack_ready = true;
                }

                if (wifi_wait_connected(15000)) {
                    ESP_LOGI(TAG, "WiFi connected successfully");
                    LOG_I("WiFi 已连接, 进入运行状态");
                    // 启动 SNTP 网络时间同步 (北京时间), 日志时间将显示为绝对时间
                    event_log_init_sntp();
                    g_system_state = SYS_STATE_RUNNING;
                } else {
                    // 【重要】连接失败不再清除 WiFi 配置、也不再进入配网模式:
                    //   1) 驱动会在断开事件里自动重连, 这里只需继续等待;
                    //   2) WiFi 长期不可用时, 服务器看门狗仍需工作 (否则服务器
                    //      失去保护), 因此在第一轮失败后就地启动 USB 看门狗。
                    ESP_LOGW(TAG, "WiFi not connected yet, keep retrying...");
                    LOG_W("WiFi 未连接成功, 保留配置继续重试");
                    start_monitoring();

                    // 连接不上时, 长按按钮仍可强制恢复出厂设置并进入配网模式 (唯一逃生通道)
                    if (gpio_is_button_pressed_long()) {
                        ESP_LOGI(TAG, "Long press detected, factory reset and restarting");
                        LOG_W("长按按钮 -> 恢复出厂设置并重启进入配网模式");
                        nvs_factory_reset();
                        esp_restart();
                    }

                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
                break;
            }

            case SYS_STATE_RUNNING: {
                LOG_I("进入运行状态");
                start_monitoring();

                // 进入正常运行循环
                while (g_system_state == SYS_STATE_RUNNING) {
                    // 喂硬件看门狗 (Task WDT)
                    if (s_task_wdt_registered) {
                        esp_task_wdt_reset();
                    }

                    // 更新 LED 状态
                    gpio_led_update();

                    // 检查复位按钮长按
                    if (gpio_is_button_pressed_long()) {
                        ESP_LOGI(TAG, "Long press detected, factory reset and restarting");
                        nvs_factory_reset();
                        esp_restart();
                    }

                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32-C3 USB Watchdog v1.0");
    ESP_LOGI(TAG, "========================================");

    // 初始化 NVS (通过 nvs_storage 模块统一初始化)
    ESP_ERROR_CHECK(nvs_storage_init());

    // 【顺序很关键】先 watchdog_init() 把内部状态清零并设默认值,
    // 再加载 NVS 中保存的参数覆盖默认值。顺序颠倒会导致
    //  watchdog_init() 内部的 memset + 默认值覆盖掉刚加载的用户参数,
    // 看起来"重启后参数丢失, 永远回到默认 60/600"。
    watchdog_init();

    // 加载保存的看门狗参数 (无保存时使用 watchdog.c 中的默认值 60/600)
    uint32_t hb_interval = 60, hb_timeout = 600;
    nvs_load_heartbeat_params(&hb_interval, &hb_timeout);
    watchdog_set_params(hb_interval, hb_timeout);
    ESP_LOGI(TAG, "Loaded watchdog params: interval=%lus, timeout=%lus",
             (unsigned long)hb_interval, (unsigned long)hb_timeout);

    // 初始化通知推送 (URL / 开关来自 NVS)
    notify_init();

    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 创建事件组
    s_wifi_event_group = xEventGroupCreate();

    // 初始化 GPIO
    gpio_control_init();

    // 初始化事件日志
    event_log_init();

    // 初始化运行时间统计
    uptime_init();

    // 初始化 OTA
    ota_update_init();

    // 硬件看门狗 (Task WDT): 任务卡死 30s 则自动重启 ESP32
    //
    // 这里【不调用】esp_task_wdt_init()。
    //
    // 原因: 只要 sdkconfig 里开了 CONFIG_ESP_TASK_WDT=y (本项目已开),
    //       IDF 会在启动阶段自动初始化 TWDT。我们再调一次会失败并打印
    //         E task_wdt: esp_task_wdt_init: TWDT already initialized
    //       这条 E 级日志容易被误认为故障, 实际功能完全正常。
    //
    // 做法: 直接沿用 IDF 初始化好的 TWDT, 只做注册 (add) 和喂狗 (reset)。
    //       - 超时时间由 CONFIG_ESP_TASK_WDT_TIMEOUT_S 决定 (本项目设为 30s)
    //       - 注册动作放在 system_task 里 (见下), 用 s_task_wdt_registered 标记结果;
    //         若 TWDT 不可用 (CONFIG_ESP_TASK_WDT=n), add 会失败, 标志位为 false,
    //         喂狗代码自动跳过, 不会有任何错误日志 —— 降级安全。
    ESP_LOGI(TAG, "Task WDT: reusing the one auto-initialized by IDF (30s timeout)");
    LOG_I("任务看门狗已启用 (30 秒超时)");

    // (watchdog_init() 已在上方完成, 不再重复调用)

    // 加载"连续多次重启后强制关机"开关 (默认开启, 可在 Web 端关闭)
    bool auto_off = true;
    nvs_load_auto_poweroff(&auto_off);
    watchdog_set_auto_poweroff(auto_off);

    // 创建系统任务
    xTaskCreate(system_task, "system_task", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "System initialized, entering main loop...");
}
