/**
 * @file captive_portal.h
 * @brief 强制门户 (Captive Portal) —— DNS 劫持部分
 *
 * 作用: 手机/电脑连上配网热点后, 系统会发起"网络连通性探测"
 *       (Android connectivitycheck.gstatic.com、iOS captive.apple.com、
 *        Windows msftconnecttest.com ...)。
 *
 *       本模块在 53 端口起一个 DNS 服务器, 把**所有**域名都解析成
 *       设备自己的 AP 地址 (192.168.4.1), 配合 smart_config 里注册的
 *       探测 URL handler, 系统就会判定"这是一个需要登录的网络",
 *       从而自动弹出配网页面 (Android 通知 / iOS CNA 窗口 / Windows 浏览器)。
 *
 * 注意: 只劫持 DNS, 不代理流量。客户端拿到 192.168.4.1 后,
 *       非探测请求会被 HTTP 服务器重定向到配网首页。
 */

#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动强制门户 DNS 劫持
 *
 * 必须在 SoftAP 启动并完成 netif 创建之后调用 (需要拿到 AP 自己的 IP)。
 * 内部会创建一个 FreeRTOS 任务监听 UDP 53, 因此是非阻塞的。
 *
 * @return esp_err_t
 */
esp_err_t captive_portal_start(void);

/**
 * @brief 停止强制门户 DNS 劫持并回收任务
 */
void captive_portal_stop(void);

/**
 * @brief 获取 AP 自身的 IPv4 地址 (网络字节序)
 *
 * 供 HTTP 层拼 302 Location 使用 (必须给绝对地址, 相对地址在
 * Android 的强制门户检测里无法打开)。
 */
uint32_t captive_portal_get_ap_ip(void);

#ifdef __cplusplus
}
#endif

#endif // CAPTIVE_PORTAL_H
