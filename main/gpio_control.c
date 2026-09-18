/**
 * @file gpio_control.c
 * @brief GPIO 控制实现: 服务器重启(复位)、服务器开机、LED、复位按钮
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "gpio_control.h"
#include "nvs_storage.h"

#define TAG "GPIO"

// 长按检测参数
#define BUTTON_ACTIVE_LEVEL    0    // 低电平有效 (按钮另一端接 GND)
#define BUTTON_POLL_MS         20   // 按钮轮询周期 (独立任务)
#define BUTTON_DEBOUNCE_MS     50   // 消抖: 电平需稳定 50ms 才认定按下/松开
#define BUTTON_HINT_MS         1000 // 按满 1s 时提示"继续按住"

// 定义在文件后部的内部函数 (供 gpio_control_init 调用)
static void button_task(void *pvParameter);
static bool power_detect_probe(void);

#define POWER_DETECT_REPROBE_MS   10000   // 接线状态重探间隔 (接线/断线都会重探)
#define POWER_DETECT_CONFIRM_CNT  2       // 连续 N 次探测结果一致才认定接线状态变化

static struct {
    bool initialized;
    led_state_t led_state;
    uint32_t last_led_toggle;
    uint32_t button_press_start;
    bool button_was_pressed;
    bool long_press_detected;
} s_gpio = {0};

// 电源检测线状态 (悬空检测)
static bool     s_pwr_detect_wired = false;
static uint32_t s_last_probe_ms    = 0;
static int      s_probe_votes      = 0;   // 连续"与当前状态相反"的探测次数

esp_err_t gpio_control_init(void)
{
    if (s_gpio.initialized) return ESP_OK;

    ESP_LOGI(TAG, "初始化 GPIO: 复位=%d, 开机=%d, 电源检测=%d, 按钮=%d, LED=%d",
             CONFIG_RESET_GPIO_PIN, CONFIG_POWERON_GPIO_PIN,
             CONFIG_POWER_DETECT_GPIO_PIN,
             CONFIG_BUTTON_GPIO_PIN, CONFIG_LED_GPIO_PIN);

    // 配置复位输出引脚 (控制服务器复位/重启)
    gpio_config_t reset_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_RESET_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&reset_cfg));
    gpio_set_level(CONFIG_RESET_GPIO_PIN, 0);  // 默认无效电平 (不触发)

    // 配置开机输出引脚 (控制服务器开机/唤醒)
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_POWERON_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pwr_cfg));
    gpio_set_level(CONFIG_POWERON_GPIO_PIN, 0);  // 默认无效电平 (不触发)

    // 配置 LED 引脚
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_LED_GPIO_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    gpio_set_level(CONFIG_LED_GPIO_PIN, 0);  // 默认关闭

    // 配置按钮引脚 (内部上拉，按下接地)
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_BUTTON_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));

    // 配置电源状态检测引脚 (接服务器 PWR_LED 信号, 输入+上拉)
    gpio_config_t pwr_det_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_POWER_DETECT_GPIO_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pwr_det_cfg));

    // 启动独立按钮任务 (长按恢复出厂), 与系统状态机解耦
    BaseType_t ret = xTaskCreate(button_task, "btn_task", 2048, NULL, 4, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return ESP_ERR_NO_MEM;
    }

    s_gpio.initialized = true;
    s_gpio.led_state = LED_OFF;
    s_gpio.long_press_detected = false;

    // 上电先探测 PWR_LED 检测线是否接入 (未接则不会误报"开机")
    s_pwr_detect_wired = power_detect_probe();
    s_last_probe_ms = (uint32_t)(esp_timer_get_time() / 1000);

    ESP_LOGI(TAG, "GPIO 初始化完成 (按钮任务已启动, 长按 %u ms 恢复出厂)",
             (unsigned)CONFIG_LONG_PRESS_DURATION);
    ESP_LOGI(TAG, "PWR_LED 检测 (GPIO%d): %s",
             CONFIG_POWER_DETECT_GPIO_PIN,
             s_pwr_detect_wired ? "已接入" : "未接入 (悬空, 状态显示为未知)");
    return ESP_OK;
}

void gpio_trigger_server_reset(uint32_t duration_ms)
{
    if (!s_gpio.initialized) return;

    ESP_LOGI(TAG, "触发服务器复位 (GPIO%d, 持续时间=%dms)",
             CONFIG_RESET_GPIO_PIN, duration_ms);

    // 拉到有效电平触发服务器复位
    gpio_set_level(CONFIG_RESET_GPIO_PIN, CONFIG_RESET_ACTIVE_LEVEL);

    // 保持一段时间让服务器检测到复位信号
    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    // 恢复无效电平
    gpio_set_level(CONFIG_RESET_GPIO_PIN, !CONFIG_RESET_ACTIVE_LEVEL);

    ESP_LOGI(TAG, "服务器复位脉冲已发送");
}

void gpio_trigger_server_poweron(uint32_t duration_ms)
{
    if (!s_gpio.initialized) return;

    if (duration_ms == 0) {
        duration_ms = CONFIG_POWERON_PULSE_MS;
    }

    ESP_LOGI(TAG, "触发服务器开机 (GPIO%d, 持续时间=%dms)",
             CONFIG_POWERON_GPIO_PIN, duration_ms);

    // 拉到有效电平触发服务器开机 (例如: 短接电源键、拉高 PWR_ON 引脚)
    gpio_set_level(CONFIG_POWERON_GPIO_PIN, CONFIG_POWERON_ACTIVE_LEVEL);

    // 保持一段时间 (模拟按住开机键)
    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    // 恢复无效电平
    gpio_set_level(CONFIG_POWERON_GPIO_PIN, !CONFIG_POWERON_ACTIVE_LEVEL);

    ESP_LOGI(TAG, "服务器开机脉冲已发送");
}

void gpio_set_led_state(led_state_t state)
{
    s_gpio.led_state = state;
}

bool gpio_is_button_pressed(void)
{
    if (!s_gpio.initialized) return false;
    return (gpio_get_level(CONFIG_BUTTON_GPIO_PIN) == BUTTON_ACTIVE_LEVEL);
}

/* ========== 按钮任务: 独立轮询 + 消抖 + 长按恢复出厂 ==========
 *
 * 之前长按检测由 system_task 轮询调用 gpio_is_button_pressed_long():
 *   - RUNNING 分支: 每 100ms 轮询, 可用;
 *   - CONNECTING 分支 (WiFi 连不上时): 一轮 = 等待 15s + 延时 5s,
 *     即 **约 20 秒才轮询一次**, 5 秒的按压几乎不可能被采到 ——
 *     而这恰恰是最需要"恢复出厂重配 WiFi"的场景, 表现为"长按很难触发"。
 *
 * 改为独立任务后与系统状态完全解耦: 任何状态 (BOOT / 配网 / 连接中 / 运行)
 * 下按住 CONFIG_LONG_PRESS_DURATION 毫秒都会触发, 并带 50ms 消抖,
 * 避免抖动/接触不良导致计时被反复清零。
 */
static void button_task(void *pvParameter)
{
    uint32_t press_start_ms = 0;
    uint32_t last_change_ms = 0;
    int      last_level     = -1;
    bool     pressed        = false;
    bool     hinted         = false;

    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        int level = gpio_get_level(CONFIG_BUTTON_GPIO_PIN);

        if (level != last_level) {
            // 电平刚变化, 重新计时消抖
            last_level = level;
            last_change_ms = now_ms;
        } else if ((now_ms - last_change_ms) >= BUTTON_DEBOUNCE_MS) {
            bool active = (level == BUTTON_ACTIVE_LEVEL);

            if (active && !pressed) {
                // 稳定按下 -> 开始计时
                pressed = true;
                press_start_ms = now_ms;
                hinted = false;
                ESP_LOGI(TAG, "按钮按下, 持续按住 %u ms 将恢复出厂设置",
                         (unsigned)CONFIG_LONG_PRESS_DURATION);
            } else if (active && pressed) {
                uint32_t held = now_ms - press_start_ms;
                if (!hinted && held >= BUTTON_HINT_MS) {
                    hinted = true;
                    ESP_LOGW(TAG, "继续按住按钮 %u ms 即可恢复出厂设置",
                             (unsigned)CONFIG_LONG_PRESS_DURATION);
                }
                if (held >= CONFIG_LONG_PRESS_DURATION) {
                    ESP_LOGW(TAG, "长按确认 (%u ms) -> 恢复出厂设置并重启", (unsigned)held);
                    nvs_factory_reset();
                    vTaskDelay(pdMS_TO_TICKS(300));   // 留出落盘/日志输出时间
                    esp_restart();
                }
            } else if (!active && pressed) {
                // 松开
                pressed = false;
                ESP_LOGI(TAG, "按钮释放 (未按满 %u ms, 取消)",
                         (unsigned)CONFIG_LONG_PRESS_DURATION);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

// LED 更新任务 (需要在主循环中定期调用，或单独任务)
void gpio_led_update(void)
{
    if (!s_gpio.initialized) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    switch (s_gpio.led_state) {
        case LED_OFF:
            gpio_set_level(CONFIG_LED_GPIO_PIN, 0);
            break;

        case LED_ON:
            gpio_set_level(CONFIG_LED_GPIO_PIN, 1);
            break;

        case LED_BLINK_SLOW:  // 500ms 间隔
            if (now_ms - s_gpio.last_led_toggle >= 500) {
                gpio_set_level(CONFIG_LED_GPIO_PIN, !gpio_get_level(CONFIG_LED_GPIO_PIN));
                s_gpio.last_led_toggle = now_ms;
            }
            break;

        case LED_BLINK_FAST:  // 100ms 间隔
            if (now_ms - s_gpio.last_led_toggle >= 100) {
                gpio_set_level(CONFIG_LED_GPIO_PIN, !gpio_get_level(CONFIG_LED_GPIO_PIN));
                s_gpio.last_led_toggle = now_ms;
            }
            break;

        case LED_BLINK_OTA:   // 200ms 间隔, 升级中提示
            if (now_ms - s_gpio.last_led_toggle >= 200) {
                gpio_set_level(CONFIG_LED_GPIO_PIN, !gpio_get_level(CONFIG_LED_GPIO_PIN));
                s_gpio.last_led_toggle = now_ms;
            }
            break;
    }
}

/* ========== 强制关机: 长按关机键 5 秒 ========== */
void gpio_force_poweroff(void)
{
    if (!s_gpio.initialized) return;

    ESP_LOGW(TAG, "强制关机: 长按开机键 %d ms (GPIO%d)",
             CONFIG_FORCE_POWEROFF_HOLD_MS, CONFIG_POWERON_GPIO_PIN);

    // 拉到有效电平, 持续 FORCE_POWEROFF_HOLD_MS 毫秒, 模拟长按电源键
    gpio_set_level(CONFIG_POWERON_GPIO_PIN, CONFIG_POWERON_ACTIVE_LEVEL);

    // 用 esp_timer 微秒级计时, 精确控制按下时长 (不受 FreeRTOS tick 粒度影响)
    uint64_t start_us = esp_timer_get_time();
    uint64_t target_us = start_us + (uint64_t)CONFIG_FORCE_POWEROFF_HOLD_MS * 1000ULL;
    while (esp_timer_get_time() < target_us) {
        // 每次让出约 5ms, 既精确又不让出太久
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // 释放按键
    gpio_set_level(CONFIG_POWERON_GPIO_PIN, !CONFIG_POWERON_ACTIVE_LEVEL);

    ESP_LOGW(TAG, "强制关机脉冲已发送 (按键已释放)");
}

/* ========== 电源状态检测 (PWR_LED) ========== */

// 悬空检测: 分别用内部上拉 / 下拉各采一次。
// 若电平始终"跟随"内部电阻 (上拉=高、下拉=低), 说明外部没有驱动源, 即检测线没接;
// 若两次电平相同 (被外部固定驱动), 说明确实接了信号。
// 背景: GPIO7 默认内部上拉, 没接线时会恒读高电平 -> Web 一直误显示"开机"。
//
// 注意: 探测会短暂 (10ms) 接内部下拉。若 PWR_LED 信号源阻抗很高 ( > ~50kΩ,
// 例如只经大电阻分压或直接接 LED 阴极), 下拉期间电平可能被拉低而被误判为
// "未接入"。建议检测线用光耦隔离或低阻分压 (总阻 ≤ 10kΩ) 后再接入。
//
// 该函数会被周期性调用 (每 POWER_DETECT_REPROBE_MS), 以便在运行中发现
// 检测线被拔掉 / 重新插上。
static bool power_detect_probe(void)
{
    gpio_pullup_en(CONFIG_POWER_DETECT_GPIO_PIN);
    gpio_pulldown_dis(CONFIG_POWER_DETECT_GPIO_PIN);
    vTaskDelay(pdMS_TO_TICKS(10));
    int with_pullup = gpio_get_level(CONFIG_POWER_DETECT_GPIO_PIN);

    gpio_pullup_dis(CONFIG_POWER_DETECT_GPIO_PIN);
    gpio_pulldown_en(CONFIG_POWER_DETECT_GPIO_PIN);
    vTaskDelay(pdMS_TO_TICKS(10));
    int with_pulldown = gpio_get_level(CONFIG_POWER_DETECT_GPIO_PIN);

    // 恢复默认: 输入 + 上拉
    gpio_pulldown_dis(CONFIG_POWER_DETECT_GPIO_PIN);
    gpio_pullup_en(CONFIG_POWER_DETECT_GPIO_PIN);
    vTaskDelay(pdMS_TO_TICKS(10));

    bool wired = (with_pullup == with_pulldown);   // 两次一致 => 被外部驱动
    ESP_LOGD(TAG, "PWR_LED 悬空检测: pullup=%d, pulldown=%d -> %s",
             with_pullup, with_pulldown, wired ? "已接线" : "悬空(未接线)");
    return wired;
}

esp_err_t gpio_power_detect_init(void)
{
    // 已在 gpio_control_init 中统一配置为输入+上拉, 这里做合法性检查
    if (!s_gpio.initialized) {
        ESP_LOGE(TAG, "电源检测初始化失败: GPIO 未初始化");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "电源状态检测已就绪 (GPIO%d, 有效电平=%d)",
             CONFIG_POWER_DETECT_GPIO_PIN, CONFIG_POWER_DETECT_ACTIVE_LEVEL);
    return ESP_OK;
}

bool gpio_power_detect_available(void)
{
    return s_pwr_detect_wired;
}

power_state_t gpio_get_power_state(void)
{
    if (!s_gpio.initialized) return POWER_STATE_UNKNOWN;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // 简单消抖: 状态变化后需稳定 DEBOUNCE_MS 才切换
    static uint32_t last_change_ms = 0;
    static int      last_raw       = -1;
    static power_state_t cached    = POWER_STATE_OFF;
    static bool      first_read    = true;   // 首次读取立即生效, 无需消抖

    // 定期重探接线状态 (无论当前是否接线):
    //  - 支持"开机后才插上检测线";
    //  - 也支持"运行中检测线被拔掉"——拔掉后引脚悬空、内部上拉恒读高电平,
    //    若不重探就会一直误显示"开机" (这正是之前只在启动时检测有效的 bug)。
    // 只在电平稳定 (超过消抖窗口) 时重探, 避免状态翻转期间误判。
    if ((now_ms - s_last_probe_ms >= POWER_DETECT_REPROBE_MS) &&
        (now_ms - last_change_ms >= CONFIG_POWER_DETECT_DEBOUNCE_MS)) {
        s_last_probe_ms = now_ms;
        bool wired = power_detect_probe();

        if (wired == s_pwr_detect_wired) {
            s_probe_votes = 0;      // 与当前状态一致, 清票
        } else if (++s_probe_votes >= POWER_DETECT_CONFIRM_CNT) {
            // 连续 N 次探测结果一致才切换, 抗偶发干扰
            s_pwr_detect_wired = wired;
            s_probe_votes = 0;
            first_read = true;      // 接线状态变了, 下次读取立即生效 (跳过消抖)
            if (wired) {
                ESP_LOGI(TAG, "PWR_LED 检测线已接入, 开始电源状态检测");
            } else {
                ESP_LOGW(TAG, "PWR_LED 检测线已断开 (引脚悬空), 电源状态显示为未知");
            }
        }
    }
    if (!s_pwr_detect_wired) {
        return POWER_STATE_UNKNOWN;   // 悬空: 不猜, 直接报"未知"
    }

    // 读 PWR_LED 引脚电平, 匹配配置的"开机有效电平"
    int level = gpio_get_level(CONFIG_POWER_DETECT_GPIO_PIN);

    if (first_read) {
        // 上电首次: 直接返回当前真实电平, 让 Web 页面立即显示正确状态
        cached = (level == CONFIG_POWER_DETECT_ACTIVE_LEVEL)
                 ? POWER_STATE_ON : POWER_STATE_OFF;
        last_raw = level;
        last_change_ms = now_ms;
        first_read = false;
        return cached;
    }

    if (level != last_raw) {
        last_raw = level;
        last_change_ms = now_ms;
    }
    if ((now_ms - last_change_ms) >= CONFIG_POWER_DETECT_DEBOUNCE_MS) {
        // 稳定超过消抖窗口, 更新缓存状态
        cached = (level == CONFIG_POWER_DETECT_ACTIVE_LEVEL)
                 ? POWER_STATE_ON : POWER_STATE_OFF;
    }

    return cached;
}
