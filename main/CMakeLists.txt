# 组件依赖说明:
#   不要添加 esp_https_ota —— 本项目 OTA 走本地 HTTP (esp_http_server + esp_ota_ops)，
#   不需要 HTTPS 客户端。引入 esp_https_ota 会连带拖入 mbedTLS/esp-tls，
#   平白多占几百 KB Flash，在 2MB 配置 (960KB/槽) 下极易导致固件放不下。
#   esp_app_format: 提供 esp_app_get_description()。
#   注意 esp_ota_get_app_description() 自 IDF v5.0 起已废弃移除，不要再用。

idf_component_register(
    SRCS
        main.c
        usb_device.c
        watchdog.c
        web_server.c
        smart_config.c
        wizard_html.c
        gpio_control.c
        captive_dns.c
        nvs_storage.c
        ota_update.c
        dashboard_html.c
        event_log.c
        uptime.c
        notify.c
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_http_server
        esp_http_client
        esp_wifi
        nvs_flash
        esp_netif
        esp_timer
        esp_driver_usb_serial_jtag
        esp_driver_gpio
        app_update
        esp_partition
        esp_app_format
        mbedtls
        lwip
)
