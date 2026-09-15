/**
 * @file web_server.h
 * @brief Web 控制端接口
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Web 服务器
 * @return esp_err_t
 */
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
