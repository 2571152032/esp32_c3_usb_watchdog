#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// USB 命令类型
typedef enum {
    USB_CMD_HEARTBEAT = 0,
    USB_CMD_ACK,
    USB_CMD_BYE,
    USB_CMD_PING,
    USB_CMD_UNKNOWN,
} usb_cmd_t;

// USB 数据包
typedef struct {
    usb_cmd_t cmd;
    char data[64];
    uint16_t len;
} usb_packet_t;

// 回调函数类型
typedef void (*usb_packet_callback_t)(const usb_packet_t *packet);
typedef void (*usb_connect_callback_t)(void);
typedef void (*usb_disconnect_callback_t)(void);

// 初始化/反初始化
esp_err_t usb_device_init(void);
esp_err_t usb_device_deinit(void);

// 连接状态
bool usb_is_connected(void);

// 发送/接收
esp_err_t usb_send_packet(const usb_packet_t *packet);
esp_err_t usb_send_string(const char *str);
esp_err_t usb_receive_packet(usb_packet_t *packet, uint32_t timeout_ms);

// 心跳
esp_err_t usb_send_heartbeat(void);

// 回调注册
void usb_register_packet_callback(usb_packet_callback_t cb);
void usb_register_connect_callback(usb_connect_callback_t cb);
void usb_register_disconnect_callback(usb_disconnect_callback_t cb);

#ifdef __cplusplus
}
#endif
