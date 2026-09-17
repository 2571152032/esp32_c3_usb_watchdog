/**
 * @file notify.h
 * @brief 事件通知推送接口 (HTTP/HTTPS Webhook)
 *
 * 方案选择: 采用 "可配置 URL 的 GET Webhook" —— 这是最简单的通用方案:
 *  - 设备端只需存一条 URL, 无需为每种推送服务写适配代码;
 *  - 兼容 bark / Server酱 / Telegram Bot / 自建 HTTP 转发等几乎所有服务;
 *  - 无需额外账号体系、APP 或 SMTP, 配置成本最低。
 *
 * URL 中可使用占位符 (发送前自动 URL 编码):
 *   {TITLE}  -> 通知标题
 *   {MSG}    -> 通知内容
 * 例:
 *   bark      https://api.day.app/<KEY>/{TITLE}/{MSG}
 *   Server酱  https://sctapi.ftqq.com/<KEY>.send?title={TITLE}&desp={MSG}
 *   Telegram  https://api.telegram.org/bot<TOKEN>/sendMessage?chat_id=<ID>&text={MSG}
 * 若 URL 中不含占位符, 则自动追加 ?title=..&msg=..
 */

#ifndef NOTIFY_H
#define NOTIFY_H

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NOTIFY_URL_MAX_LEN   192

/**
 * @brief 初始化通知模块 (从 NVS 读取开关与推送地址)
 */
esp_err_t notify_init(void);

/**
 * @brief 通知功能是否启用
 */
bool notify_is_enabled(void);

/**
 * @brief 获取当前推送地址 (可能为空字符串)
 */
const char *notify_get_url(void);

/**
 * @brief 保存通知配置 (生效并写入 NVS)
 * @param enabled 是否启用
 * @param url     推送地址, NULL 表示保持当前值
 */
esp_err_t notify_set_config(bool enabled, const char *url);

/**
 * @brief 异步发送通知 (内部创建临时任务, 不阻塞调用者)
 * @note  未启用 / 地址为空 / 上一次仍在发送时会直接跳过
 */
void notify_send_async(const char *title, const char *text);

#ifdef __cplusplus
}
#endif

#endif // NOTIFY_H
