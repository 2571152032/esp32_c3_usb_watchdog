/**
 * @file captive_dns.h
 * @brief 配网用的强制门户 DNS 服务器
 *
 * 作用: 设备连上配网热点 (Watchdog-AP) 后, 系统会发起"联网探测"
 *       (Android 的 connectivitycheck.gstatic.com、iOS 的 captive.apple.com、
 *        Windows 的 msftconnecttest.com 等)。
 *
 *       本模块劫持所有 DNS 查询, 一律回答本机的 AP 地址, 于是探测请求都会
 *       落到我们的 HTTP 服务器上; 再配合 smart_config.c 里的 404 重定向,
 *       系统就会弹出"需要登录/进入网络"的提示, 点击直达配网页面。
 */

#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 DNS 服务器
 * @param ap_ip_addr AP 接口的 IPv4 地址 (网络字节序, 即 ip4_addr_t.addr 原值)
 */
esp_err_t captive_dns_start(uint32_t ap_ip_addr);

/**
 * @brief 停止 DNS 服务器
 */
void captive_dns_stop(void);

#ifdef __cplusplus
}
#endif

#endif // CAPTIVE_DNS_H
