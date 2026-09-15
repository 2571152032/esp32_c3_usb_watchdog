/**
 * @file dashboard_html.h
 * @brief 美化后的 Web 控制台 HTML 页面
 *
 * 通过 dashboard_get_html() 获取页面模板字符串 (含 %% 占位符),
 * 由 web_server.c 的 handler_dashboard 负责注入动态值。
 */

#ifndef DASHBOARD_HTML_H
#define DASHBOARD_HTML_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 返回美化后的控制台 HTML 模板
 * @return const char* 静态字符串 (含 %s / %lu 占位符)
 */
const char *dashboard_get_html(void);

#ifdef __cplusplus
}
#endif

#endif // DASHBOARD_HTML_H
