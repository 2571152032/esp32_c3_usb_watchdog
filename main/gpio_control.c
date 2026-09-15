/**
 * @file gpio_control.c
 * @brief GPIO 控制实现: 服务器重启(复位)、服务器开机、LED、复位按钮
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gpio_control.h"

#define TAG "GPIO"

// 长按检测参数
#define BUTTON_ACTIVE_LEVEL    0    // 低电平有效 (按钮另一端接 GND)

static struct {
    bool initialized;
    led_state_t led_state;
    uint32_t last_led_toggle;
    uint32_t button_press_start;
    bool button_was_pressed;
    bool long_press_detected;
} s_gpio = {0};

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

    s_gpio.initialized = true;
    s_gpio.led_state = LED_OFF;
    s_gpio.long_press_detected = false;

    ESP_LOGI(TAG, "GPIO 初始化完成");
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

bool gpio_is_button_pressed_long(void)
{
    static uint32_t press_start_time = 0;
    static bool press_detected = false;

    if (gpio_is_button_pressed()) {
        if (!press_detected) {
            press_start_time = (uint32_t)(esp_timer_get_time() / 1000);
            press_detected = true;
        }

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t press_duration = now - press_start_time;

        if (press_duration >= CONFIG_LONG_PRESS_DURATION) {
            // 重置，防止重复触发
            press_detected = false;
            press_start_time = 0;
            ESP_LOGI(TAG, "检测到长按 (%ums)", press_duration);
            return true;
        }
    } else {
        press_detected = false;
        press_start_time = 0;
    }

    return false;
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

power_state_t gpio_get_power_state(void)
{
    if (!s_gpio.initialized) return POWER_STATE_OFF;

    // 读 PWR_LED 引脚电平, 匹配配置的"开机有效电平"
    int level = gpio_get_level(CONFIG_POWER_DETECT_GPIO_PIN);

    // 简单消抖: 状态变化后需稳定 DEBOUNCE_MS 才切换
    static uint32_t last_change_ms = 0;
    static int      last_raw       = -1;
    static power_state_t cached    = POWER_STATE_OFF;
    static bool      first_read    = true;   // 首次读取立即生效, 无需消抖
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

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
