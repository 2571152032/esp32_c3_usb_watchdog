#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "usb_device.h"

static const char *TAG = "usb_device";

static TaskHandle_t s_usb_rx_task_handle = NULL;
static usb_packet_callback_t s_packet_callback = NULL;
static usb_connect_callback_t s_connect_callback = NULL;
static usb_disconnect_callback_t s_disconnect_callback = NULL;
static SemaphoreHandle_t s_tx_mutex = NULL;
static bool s_driver_installed = false;

#define USB_RX_BUF_SIZE   256
#define USB_LINE_BUF_SIZE 256

static char s_rx_buf[USB_RX_BUF_SIZE];
static char s_line_buf[USB_LINE_BUF_SIZE];
static size_t s_line_len = 0;    // 当前 line_buf 中未处理字节数

// 解析并分发一行数据
static void process_line(char *line)
{
    if (strlen(line) == 0) return;
    ESP_LOGD(TAG, "USB RX: '%s'", line);

    usb_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    if (strncmp(line, "ZAIMA", 5) == 0) {
        packet.cmd = USB_CMD_HEARTBEAT;
    } else if (strncmp(line, "HEI-ZAIDE", 9) == 0) {
        // 服务器回复的心跳应答
        packet.cmd = USB_CMD_ACK;
    } else if (strncmp(line, "ACK", 3) == 0) {
        packet.cmd = USB_CMD_ACK;
    } else if (strncmp(line, "BYE", 3) == 0) {
        packet.cmd = USB_CMD_BYE;
    } else if (strncmp(line, "PING", 4) == 0) {
        packet.cmd = USB_CMD_PING;
    } else {
        packet.cmd = USB_CMD_UNKNOWN;
    }

    strncpy(packet.data, line, sizeof(packet.data) - 1);
    packet.len = strlen(line);

    if (s_packet_callback) {
        s_packet_callback(&packet);
    }
}

static void usb_rx_task(void *arg)
{
    int len;
    ESP_LOGI(TAG, "USB RX task started");

    while (1) {
        len = usb_serial_jtag_read_bytes(s_rx_buf, USB_RX_BUF_SIZE - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            // 将新数据追加到行缓冲, 不要直接处理"尾行"——它可能是被截断的不完整行,
            // 直接当成完整行解析会导致 ZAIMA/HEI-ZAIDE 被误判为 UNKNOWN 而丢失。
            if ((size_t)len >= USB_LINE_BUF_SIZE - s_line_len) {
                // 行缓冲溢出, 丢弃旧数据避免死锁
                ESP_LOGW(TAG, "Line buffer overflow, dropping %d bytes", (int)s_line_len);
                s_line_len = 0;
            }
            if ((size_t)len < USB_LINE_BUF_SIZE - s_line_len) {
                memcpy(s_line_buf + s_line_len, s_rx_buf, len);
                s_line_len += len;
                s_line_buf[s_line_len] = '\0';
            }

            // 只在遇到 \n/\r 时才把前面的内容作为完整行处理, 剩余未结束部分留到下一次读取。
            char *start = s_line_buf;
            for (size_t i = 0; i < s_line_len; i++) {
                if (s_line_buf[i] == '\n' || s_line_buf[i] == '\r') {
                    s_line_buf[i] = '\0';
                    process_line(start);
                    start = &s_line_buf[i + 1];
                }
            }

            size_t remaining = s_line_len - (size_t)(start - s_line_buf);
            if (remaining > 0 && start != s_line_buf) {
                memmove(s_line_buf, start, remaining);
            }
            s_line_len = remaining;
            s_line_buf[s_line_len] = '\0';
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t usb_device_init(void)
{
    if (s_driver_installed) {
        ESP_LOGI(TAG, "USB driver already installed, skipping");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing USB CDC-ACM (ESP32-C3 built-in USB)...");

    // TX 互斥锁必须在任何 "installed" 返回路径之前创建:
    // 驱动被控制台占用走 "already installed" 分支时同样要能安全发送,
    // 否则 usb_send_string() 里 xSemaphoreTake(NULL) 会直接崩溃。
    if (!s_tx_mutex) {
        s_tx_mutex = xSemaphoreCreateMutex();
        if (!s_tx_mutex) {
            ESP_LOGE(TAG, "Failed to create TX mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };

    esp_err_t ret = usb_serial_jtag_driver_install(&cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "USB driver already installed");
        s_driver_installed = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Enable USB CDC in menuconfig:");
        ESP_LOGE(TAG, "  Component config -> USB (USB-OTG) -> Enable USB CDC");
        return ret;
    }

    s_driver_installed = true;

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        usb_rx_task, "usb_rx", 4096, NULL, 5, &s_usb_rx_task_handle, 0);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB RX task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB CDC-ACM initialized");
    ESP_LOGI(TAG, "  D- = GPIO18, D+ = GPIO19");
    ESP_LOGI(TAG, "  Linux: /dev/ttyACM*, Windows: COM*");
    return ESP_OK;
}

esp_err_t usb_device_deinit(void)
{
    if (!s_driver_installed) return ESP_OK;

    if (s_usb_rx_task_handle) {
        vTaskDelete(s_usb_rx_task_handle);
        s_usb_rx_task_handle = NULL;
    }
    if (s_tx_mutex) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
    }

    usb_serial_jtag_driver_uninstall();
    s_driver_installed = false;
    ESP_LOGI(TAG, "USB device deinitialized");
    return ESP_OK;
}

bool usb_is_connected(void)
{
    return s_driver_installed;
}

esp_err_t usb_send_packet(const usb_packet_t *packet)
{
    if (!s_driver_installed || !packet) return ESP_ERR_INVALID_STATE;

    const char *str;
    switch (packet->cmd) {
        case USB_CMD_HEARTBEAT: str = "ZAIMA\n"; break;
        case USB_CMD_ACK:       str = "HEI-ZAIDE\n"; break;
        case USB_CMD_BYE:       str = "BYE\n"; break;
        case USB_CMD_PING:      str = "PING\n"; break;
        default:                str = packet->data; break;
    }
    return usb_send_string(str);
}

esp_err_t usb_send_string(const char *str)
{
    if (!s_driver_installed || !str) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    size_t total = strlen(str);
    size_t sent = 0;
    int zero_rounds = 0;
    while (sent < total) {
        int written = usb_serial_jtag_write_bytes(str + sent, total - sent, pdMS_TO_TICKS(100));
        if (written < 0) {
            xSemaphoreGive(s_tx_mutex);
            return ESP_FAIL;
        }
        if (written == 0) {
            // 主机端未取走数据 (服务器已关机 / USB 未插好):
            // 最多重试约 2 秒后放弃, 避免看门狗任务在此永久阻塞、超时检测失效
            if (++zero_rounds > 20) {
                xSemaphoreGive(s_tx_mutex);
                ESP_LOGW(TAG, "TX stalled (host not reading), drop %u bytes", (unsigned)(total - sent));
                return ESP_ERR_TIMEOUT;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        zero_rounds = 0;
        sent += written;
    }
    xSemaphoreGive(s_tx_mutex);
    return ESP_OK;
}

esp_err_t usb_receive_packet(usb_packet_t *packet, uint32_t timeout_ms)
{
    if (!packet) return ESP_ERR_INVALID_ARG;

    int len = usb_serial_jtag_read_bytes(s_rx_buf, USB_RX_BUF_SIZE - 1, pdMS_TO_TICKS(timeout_ms));
    if (len <= 0) return ESP_ERR_TIMEOUT;

    s_rx_buf[len] = '\0';
    memset(packet, 0, sizeof(usb_packet_t));

    if (strncmp(s_rx_buf, "HEI-ZAIDE", 9) == 0 || strncmp(s_rx_buf, "ACK", 3) == 0) {
        packet->cmd = USB_CMD_ACK;
    } else if (strncmp(s_rx_buf, "BYE", 3) == 0) {
        packet->cmd = USB_CMD_BYE;
    } else if (strncmp(s_rx_buf, "PING", 4) == 0) {
        packet->cmd = USB_CMD_PING;
    } else {
        packet->cmd = USB_CMD_UNKNOWN;
    }

    strncpy(packet->data, s_rx_buf, sizeof(packet->data) - 1);
    packet->len = (uint16_t)len;
    return ESP_OK;
}

void usb_register_packet_callback(usb_packet_callback_t cb) { s_packet_callback = cb; }
void usb_register_connect_callback(usb_connect_callback_t cb) { s_connect_callback = cb; }
void usb_register_disconnect_callback(usb_disconnect_callback_t cb) { s_disconnect_callback = cb; }

esp_err_t usb_send_heartbeat(void)
{
    return usb_send_string("ZAIMA\n");
}
