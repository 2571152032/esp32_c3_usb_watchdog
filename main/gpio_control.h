/**
 * @file gpio_control.h
 * @brief GPIO 控制: 服务器重启(复位)、服务器开机、强制关机、电源状态检测、LED、复位按钮
 */

#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// 从 menuconfig 获取的 GPIO 定义 (默认)
#ifndef CONFIG_RESET_GPIO_PIN
#define CONFIG_RESET_GPIO_PIN    4   // 复位/重启引脚
#endif
#ifndef CONFIG_POWERON_GPIO_PIN
#define CONFIG_POWERON_GPIO_PIN  3   // 开机/唤醒/强制关机引脚 (共用, 靠时长区分)
#endif
#ifndef CONFIG_POWER_DETECT_GPIO_PIN
#define CONFIG_POWER_DETECT_GPIO_PIN 7  // 电源状态检测 (接服务器 PWR_LED 信号)
#endif
#ifndef CONFIG_BUTTON_GPIO_PIN
#define CONFIG_BUTTON_GPIO_PIN   5   // 配网按钮
#endif
#ifndef CONFIG_LED_GPIO_PIN
#define CONFIG_LED_GPIO_PIN      6   // 状态 LED
#endif
#ifndef CONFIG_LONG_PRESS_DURATION
#define CONFIG_LONG_PRESS_DURATION  5000
#endif

// 引脚有效电平配置 (可根据实际电路修改)
#ifndef CONFIG_RESET_ACTIVE_LEVEL
#define CONFIG_RESET_ACTIVE_LEVEL  1   // 复位引脚有效电平 (1=高电平触发)
#endif
#ifndef CONFIG_POWERON_ACTIVE_LEVEL
#define CONFIG_POWERON_ACTIVE_LEVEL 1  // 开机/关机引脚有效电平 (1=高电平触发)
#endif
#ifndef CONFIG_POWER_DETECT_ACTIVE_LEVEL
#define CONFIG_POWER_DETECT_ACTIVE_LEVEL 1  // 电源检测有效电平 (PWR_LED 亮=开机)
#endif

// 开机脉冲默认时长 (毫秒)
#ifndef CONFIG_POWERON_PULSE_MS
#define CONFIG_POWERON_PULSE_MS  500
#endif

// 强制关机: 长按关机键的持续时间 (毫秒)
#ifndef CONFIG_FORCE_POWEROFF_HOLD_MS
#define CONFIG_FORCE_POWEROFF_HOLD_MS  5000  // 长按 5 秒强制关机
#endif

// 电源状态检测消抖时长 (毫秒)
#ifndef CONFIG_POWER_DETECT_DEBOUNCE_MS
#define CONFIG_POWER_DETECT_DEBOUNCE_MS 50
#endif

// LED 状态
typedef enum {
    LED_OFF,
    LED_ON,
    LED_BLINK_SLOW,    // 1Hz
    LED_BLINK_FAST,    // 5Hz
    LED_BLINK_OTA,     // OTA 升级中, 超快闪 (200ms)
} led_state_t;

// 服务器电源状态 (通过 PWR_LED 检测)
typedef enum {
    POWER_STATE_OFF = 0,   // 关机 / 休眠 (PWR_LED 灭)
    POWER_STATE_ON  = 1,   // 开机 (PWR_LED 亮)
} power_state_t;

/**
 * @brief 初始化 GPIO (复位、开机、电源检测、LED、按钮)
 */
esp_err_t gpio_control_init(void);

/**
 * @brief 触发服务器重启 (拉高/拉低复位引脚一段时间)
 * @param duration_ms 保持时间 (毫秒)
 */
void gpio_trigger_server_reset(uint32_t duration_ms);

/**
 * @brief 触发服务器开机 (短脉冲, 模拟按一下开机键)
 * @param duration_ms 保持时间 (毫秒), 0 表示使用默认值
 */
void gpio_trigger_server_poweron(uint32_t duration_ms);

/**
 * @brief 强制关机 (长按关机键 5 秒)
 *        拉高开机引脚并保持 CONFIG_FORCE_POWEROFF_HOLD_MS 毫秒后释放
 */
void gpio_force_poweroff(void);

/**
 * @brief 初始化电源状态检测引脚 (把 PWR_LED 引脚配为输入+上拉)
 */
esp_err_t gpio_power_detect_init(void);

/**
 * @brief 读取服务器电源状态 (通过 PWR_LED 电平)
 * @return POWER_STATE_ON / POWER_STATE_OFF
 */
power_state_t gpio_get_power_state(void);

/**
 * @brief 设置 LED 状态
 */
void gpio_set_led_state(led_state_t state);

/**
 * @brief 读取复位按钮状态
 * @return true 按下, false 释放
 */
bool gpio_is_button_pressed(void);

/**
 * @brief 检测按钮是否长按 (>= 5秒)
 * @return true 长按检测到
 */
bool gpio_is_button_pressed_long(void);

/**
 * @brief LED 状态更新 (在主循环中定期调用)
 */
void gpio_led_update(void);

#ifdef __cplusplus
}
#endif

#endif // GPIO_CONTROL_H
