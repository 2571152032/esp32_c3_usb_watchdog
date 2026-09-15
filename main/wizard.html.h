/**
 * @file wizard.html.h
 * @brief 配网向导页面 (手动输入 SSID/密码，无扫描)
 *
 * 极简静态页面，避免 JS/AJAX 解析导致 500。
 * 使用标准 form POST (application/x-www-form-urlencoded)，
 * 后端用 httpd_query_key_value 解析，最稳定。
 */

#ifndef WIZARD_HTML_H
#define WIZARD_HTML_H

/* 页面是纯字符串常量，通过 wizard_get_html() 返回 */
const char *wizard_get_html(void);

#endif // WIZARD_HTML_H
