/**
 * @file notify.h
 * @brief 事件通知推送接口 (HTTP/HTTPS POST + Bearer 鉴权)
 *
 * 推送方式 (v1.4):
 *   POST <服务地址>/n
 *   Content-Type: application/json
 *   Authorization: Bearer <token>       (同时附 X-Auth-Token: <token> 以兼容其他接收端)
 *   Body: {"t":"<标题>","m":"<内容>","k":"<token>","event":"server_down","ts":1234567890}
 *
 * - 服务地址填 Base URL (如 https://example.com), 设备自动追加 /n;
 *   若地址本身已以 /n 结尾则不重复追加。
 * - t / m 为自建服务 (Flask/Go 等) 最常用的"标题/内容"字段名;
 *   k 与头部的 Authorization / X-Auth-Token 三选一即可, 接收端按需取用。
 * - event 为事件类型, 便于接收端程序按类型分支处理:
 *     server_down       服务器宕机, 已触发重启
 *     force_poweroff    连续重启未恢复, 已强制关机
 *     watchdog_stopped  1 小时内重启过多, 看门狗已停止
 *     power_on          管理员执行开机, 监控恢复
 *     web_reboot        Web 控制台触发服务器重启
 *     web_poweroff      Web 控制台触发强制关机
 *     web_shutdown      Web 控制台触发软关机
 *     device_reboot     Web 控制台触发看门狗设备自身重启
 *     test              Web 端"测试通知"
 *     generic           其他
 * - ts 为 Unix 时间戳 (未校时为 0)。
 * - 兼容: 若服务地址中仍含 {TITLE} / {MSG} 占位符 (旧版 Webhook 配置),
 *   会自动回退到旧的 GET + 占位符替换方式, 老配置升级后不会失效。
 */

#ifndef NOTIFY_H
#define NOTIFY_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NOTIFY_URL_MAX_LEN     192
#define NOTIFY_TOKEN_MAX_LEN   96
#define NOTIFY_EVENT_MAX_LEN   24

/** 预定义事件类型 (notify.c 中亦有同名字符串常量表) */
#define NOTIFY_EVENT_SERVER_DOWN      "server_down"
#define NOTIFY_EVENT_FORCE_POWEROFF   "force_poweroff"
#define NOTIFY_EVENT_WATCHDOG_STOPPED "watchdog_stopped"
#define NOTIFY_EVENT_POWER_ON         "power_on"
#define NOTIFY_EVENT_TEST             "test"
#define NOTIFY_EVENT_GENERIC          "generic"
#define NOTIFY_EVENT_WEB_REBOOT       "web_reboot"
#define NOTIFY_EVENT_WEB_POWEROFF     "web_poweroff"
#define NOTIFY_EVENT_WEB_SHUTDOWN     "web_shutdown"
#define NOTIFY_EVENT_DEVICE_REBOOT    "device_reboot"

/**
 * @brief 初始化通知模块 (从 NVS 读取开关 / 服务地址 / 令牌)
 */
esp_err_t notify_init(void);

/**
 * @brief 通知功能是否启用
 */
bool notify_is_enabled(void);

/**
 * @brief 获取通知服务地址 (可能为空字符串)
 */
const char *notify_get_url(void);

/**
 * @brief 获取鉴权令牌 (可能为空字符串)
 */
const char *notify_get_token(void);

/**
 * @brief 保存通知配置 (生效并写入 NVS)
 * @param enabled 是否启用
 * @param url     服务地址, NULL 表示保持当前值
 * @param token   令牌, NULL 表示保持当前值
 */
esp_err_t notify_set_config(bool enabled, const char *url, const char *token);

/**
 * @brief 异步发送通知 (内部创建临时任务, 不阻塞调用者)
 * @param event 事件类型 (见 NOTIFY_EVENT_* 宏), NULL 时用 generic
 * @param title 通知标题
 * @param text  通知内容
 * @note  未启用 / 地址为空 / 上一次仍在发送时会直接跳过
 */
void notify_send_event(const char *event, const char *title, const char *text);

/**
 * @brief 兼容旧接口: 等价于 notify_send_event(NULL, title, text)
 */
void notify_send_async(const char *title, const char *text);

#ifdef __cplusplus
}
#endif

#endif // NOTIFY_H
