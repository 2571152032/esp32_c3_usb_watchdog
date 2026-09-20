# ESP32-C3 USB Watchdog

Linux 服务器硬件看门狗：USB CDC-ACM 心跳 + GPIO 硬件控制 + Web 控制台 + OTA 固件更新。

## 功能

- **USB CDC-ACM 心跳**：默认每 60 秒发一次 `ZAIMA`，10 分钟（600s）无响应触发 GPIO 复位
- **GPIO 控制**：服务器复位 / 开机 / 强制关机 / 电源状态检测（PWR_LED）/ LED / 配网按钮
- **配网（强制门户 Captive Portal）**：AP 热点 + DNS 劫持，连上热点自动弹出配网页 + WiFi 扫描选择
- **Web 控制台**（深色玻璃拟态主题）：状态监控、心跳统计、参数设置、凭据管理、**固件 OTA 更新**、实时事件日志
- **Web 认证**：默认 `admin / admin123`，可修改；重置网络后恢复默认值
- **事件通知推送**（可选）：HTTP(S) GET Webhook，宕机 / 强制关机 / 看门狗停止时推送
- **OTA 双分区**（factory + ota_0 + ota_1），支持失败自动回滚

## GPIO 接线与功能（硬件接线必读）

### 引脚总览

| GPIO | 方向 | 宏定义（`main/gpio_control.h`） | 功能 | 电平 / 时序 |
|---|---|---|---|---|
| **GPIO4** | 输出 | `CONFIG_RESET_GPIO_PIN` | 服务器**复位（重启）**脉冲 | 默认高有效，判宕后输出 **500 ms** 脉冲 |
| **GPIO3** | 输出 | `CONFIG_POWERON_GPIO_PIN` | 服务器**开机**（短按）/ **强制关机**（长按，同一引脚靠时长区分） | 默认高有效；开机 **500 ms** 脉冲，强制关机按住 **5 s** |
| **GPIO7** | 输入（内部上拉） | `CONFIG_POWER_DETECT_GPIO_PIN` | 服务器**电源状态检测**（接 PWR_LED 信号） | 高电平 = 开机；消抖 **50 ms** |
| **GPIO5** | 输入（内部上拉） | `CONFIG_BUTTON_GPIO_PIN` | **配网 / 恢复出厂按钮** | 低有效（按下接地），长按 **5 s** 恢复出厂设置并重启 |
| **GPIO6** | 输出 | `CONFIG_LED_GPIO_PIN` | **状态 LED** | 高电平点亮；慢闪 1 Hz / 快闪 5 Hz / OTA 中 200 ms |
| **GPIO18 / GPIO19** | USB | — | 内置 USB D- / D+（**CDC-ACM 心跳通道**） | 接服务器 USB 口，**不可复用为普通 GPIO** |

> ⚠ ESP32-C3 的 Strapping 引脚是 **GPIO2 / GPIO8 / GPIO9**，上表分配已全部避开；
> GPIO18/19 是 USB Serial/JTAG 专用。改动引脚时请避开这些。

### 接线说明

**1. GPIO4 → 服务器主板 `RESET`（复位）排针**

- 一端接主板 `RESET SW` 的其中一根针，另一端接主板 `GND`（**必须与 ESP32-C3 共地**）
- 建议经 **光耦 / NPN 三极管 / 小信号 MOSFET** 驱动（等效“按一下复位键”），避免把主板电平直接引入 MCU
- 默认高电平有效（`CONFIG_RESET_ACTIVE_LEVEL=1`）；低有效主板改该宏为 `0`

**2. GPIO3 → 服务器主板 `POWER SW`（开机键）排针**

- 接法同上（一端 `PWR SW`，一端 `GND`），与 GPIO4 共用主板 GND
- **开机**：Web 点「开机（脉冲）」或管理员介入时输出 500 ms 脉冲（模拟短按）
- **强制关机**：连续多次重启未恢复时自动执行，或手动点「强制关机」——同一引脚保持有效电平 **5 秒**（模拟长按电源键）

**3. GPIO7 → 服务器主板 `PWR_LED`（电源指示灯）**

- 接 PWR_LED 的**正极信号**（部分主板为 `PWR LED +`），并与主板共地；引脚内部已上拉
- **电平务必 ≤ 3.3 V**：主板 PWR_LED 若是 5 V 电平，必须经过**电阻分压或光耦隔离**后再接入，否则会烧毁 ESP32-C3
- 默认高电平 = 开机（`CONFIG_POWER_DETECT_ACTIVE_LEVEL=1`）；若主板 PWR_LED 为低有效，改该宏为 `0`
- Web 控制台「服务器电源状态」每 3 秒刷新，读取的就是这个引脚

**4. GPIO5 → 配网 / 恢复出厂按钮**

- 按钮两端分别接 **GPIO5** 与 **GND**（内部已上拉，低电平有效，无需外接电阻）
- 长按 **5 秒** → 清空 WiFi / 凭据 / 心跳参数并重启进入配网模式（Web 端「恢复出厂设置」按钮等效）
- 启动阶段与运行期都会检测，是设备“救砖”的唯一本地通道

**5. GPIO6 → 状态 LED**

- 串联 **330 Ω ~ 1 kΩ** 电阻接 LED 正极到 GPIO6（LED 负极接地）；若 LED 为高亮/大功率，改用三极管驱动
- 状态含义：常亮 / 慢闪（1 Hz，正常监控）/ **快闪（5 Hz，需管理员介入**——强制关机或 1 小时内重启过多）/ 200 ms 超快闪（OTA 升级中）

**6. USB（GPIO18 / GPIO19）→ 服务器 USB 口**

- 直接用数据线连服务器 USB-A 口，设备枚举为 `/dev/ttyACM*`，用于心跳 `ZAIMA` 收发
- 与服务器**同一台机器**的 USB 连接是判定存活的依据，插在别的机器上等于一直“宕机”

### 接线示意

```text
                     服务器主板
              ┌──── RESET SW ────┐
              │                  │
   GPIO4 ─────┘ (光耦/MOSFET)    │      3.3V
                                 │       │
              ┌──── POWER SW ────┐      [ ] 上拉(内部)
   GPIO3 ─────┘ (光耦/MOSFET)    │       │
                                 │   GPIO7 ──── PWR_LED+ (≤3.3V, 5V 需分压)
              ┌──── PWR_LED+ ────┘       GPIO5 ──── 按钮 ──── GND
              │                          GPIO6 ──── LED ──[330Ω]── GND
             GND ─────────────── 共地 ─────────────── GND
                                          USB (GPIO18/19) ──── 服务器 USB 口
```

### 修改引脚

本项目未提供 Kconfig，引脚是 `main/gpio_control.h` 中 `#ifndef` 保护的编译期默认值。
如需改动，直接修改对应宏（若你自行在 menuconfig 中定义了同名 `CONFIG_*`，以 menuconfig 为准）：

```c
#define CONFIG_RESET_GPIO_PIN          4   // 复位
#define CONFIG_POWERON_GPIO_PIN        3   // 开机 / 强制关机
#define CONFIG_POWER_DETECT_GPIO_PIN   7   // 电源检测 (PWR_LED)
#define CONFIG_BUTTON_GPIO_PIN         5   // 配网按钮
#define CONFIG_LED_GPIO_PIN            6   // 状态 LED
```

配套的时序 / 有效电平同样在该文件调整：`CONFIG_POWERON_PULSE_MS`（开机脉冲）、
`CONFIG_FORCE_POWEROFF_HOLD_MS`（强制关机长按）、`CONFIG_LONG_PRESS_DURATION`（按钮长按）、
`CONFIG_*_ACTIVE_LEVEL`（各引脚有效电平）、`CONFIG_POWER_DETECT_DEBOUNCE_MS`（消抖）。

> 接线前请再次确认：**共地、电平 ≤ 3.3 V、不占用 GPIO2/8/9 与 18/19**。

## 目录结构

```text
esp32_c3_usb_watchdog/
├── CMakeLists.txt           # 项目注册 (PROJECT_VER = 1.3.2)
├── partitions.csv           # OTA 双分区表 (4MB Flash 版, 默认)
├── sdkconfig.defaults       # 默认配置
├── server/
│   ├── install.sh           # 一键安装脚本
│   ├── usb-watchdog.py      # 服务器端主程序
│   ├── usb-watchdog.service # Linux服务器端心跳回复服务
├── README.md
├── demo/
│   └── dashboard_demo.html  # 纯前端演示页 (浏览器直接打开, 无需设备)
└── main/
    ├── CMakeLists.txt       # 组件注册
    ├── main.c               # 入口 / system_task 状态机
    ├── usb_device.c/.h      # USB CDC-ACM 心跳收发
    ├── watchdog.c/.h        # 看门狗逻辑 (60s / 600s)
    ├── web_server.c/.h      # Web 控制台 + 认证 + OTA 路由
    ├── dashboard_html.c/.h  # 控制台 UI 模板
    ├── ota_update.c/.h      # 固件 OTA 上传 / 校验 / 写入 / 回滚
    ├── smart_config.c/.h    # AP 配网 + 强制门户探测路由
    ├── captive_portal.c/.h  # 强制门户 DNS 劫持 (UDP 53)
    ├── wizard_html.c/.h     # 配网向导页面
    ├── gpio_control.c/.h    # GPIO 控制 (复位/开机/关机/电源检测/LED/按钮)
    ├── nvs_storage.c/.h     # NVS (WiFi + 凭据 + 心跳参数)
    ├── notify.c/.h          # 事件通知推送 (HTTP/HTTPS Webhook)
    ├── event_log.c/.h       # 事件日志 (RAM + NVS)
    └── uptime.c/.h          # 运行时间 / 重启计数
```

## 构建（ESP-IDF v6.1）

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

**版本号**：根 `CMakeLists.txt` 中的 `set(PROJECT_VER "1.3.2")` 控制。发版时改这一处即可，
版本号与构建日期会自动写入镜像 `esp_app_desc_t`，Web 控制台页头显示
`版本 v1.3.2 · 编译日期 Sep 18 2026`，OTA 上传页解析的也是同一字段。

> 改过 `sdkconfig.defaults` 或 `partitions.csv` 后，建议 `idf.py fullclean` 再 `build`，
> 否则 CMake 可能沿用旧缓存导致新配置不生效。

## Flash 大小与分区表（重要）

本项目**默认按 4MB Flash 配置**（`partitions.csv` + `sdkconfig.defaults` 已配套，
开箱即用，无需手工切换）。分区表为 `ota_0 + ota_1` 双槽、**不含 factory**：

| 文件 | 适用 Flash | 每个 OTA 槽 | 布局 |
|---|---|---|---|
| `partitions.csv` | **4MB（默认）** | `0x1F0000` = **1984 KB** | 0x20000 – 0x400000 |

**固件大小不得超过所在槽的大小**，否则报 `app partition is too small for binary`。

- 4MB 方案 → 占用率 **47%**，余量充足

## menuconfig 关键配置

- **Serial Flasher Config** → Flash size = **4MB**（与 `partitions.csv` 配套）
- **Partition Table** → `Custom partition table CSV` → `partitions.csv`
- **Bootloader** → `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
  （新固件启动后若未标记 valid 或反复崩溃，自动切回上一分区）
- **Bootloader** → anti-rollback 已**关闭**
  （`# CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK is not set`）
- **USB CDC**：`CONFIG_USB_CDC_ENABLED=y`（USB Serial/JTAG 控制台）

### 为什么关闭 anti-rollback？

它要求新固件 `secure_version` 严格递增，否则**永久拒绝启动**（只能串口救砖）。
对本项目「Web 手动上传固件」场景风险大于收益；普通 rollback 已能兜住
「新固件起不来」的情况，更安全。

如确需防降级：设为 `=y`，并在每次发版时递增
`CONFIG_BOOTLOADER_APP_SECURE_VERSION`。

## 配网（强制门户，自动弹出）

1. 上电，首次启动进入 AP 配网模式：热点 `Watchdog-AP` / 密码 `12345678`（`main/smart_config.c`）
2. 手机 / 电脑连上该热点后**会自动弹出配网页面**（Android 通知、iOS/macOS CNA 窗口、Windows 浏览器）
3. 页面会自动扫描附近 WiFi，点一下即可填入 SSID，**也可手动输入**
4. 保存后设备重启并连接该 WiFi，访问其 IP 进入控制台（默认账号 `admin / admin123`）

> 若系统没有自动弹出（部分定制 ROM / 关闭了强制门户检测），
> 手动打开 `http://192.168.4.1` 即可，功能完全一致。

### 实现要点

| 组件 | 作用 |
|---|---|
| `captive_portal.c` | UDP 53 迷你 DNS 服务器：**所有**域名的 A 记录都回答 AP 自身 IP；AAAA 查询回 `NOERROR` + 空回答（让客户端回落到 IPv4） |
| `smart_config.c` | 注册各系统探测 URL：`/generate_204*`（Android）返回 302，`/hotspot-detect*`（Apple）返回 200 + 页面，`/ncsi.txt*`（Windows）等；末尾注册通配 `*` 兜底重定向 |
| `wizard_html.c` | 配网页：`GET /api/scan` 拉附近 WiFi 列表，点击填入，失败自动回退手动输入 |

**为什么 Android 用 302 而不是 204**：`generate_204` 期待 204 表示"有网"。
回 302 才会让系统判定为"需要登录的网络"并弹出通知 —— 这正是我们要的。

**为什么 Apple 要回页面而不是 302**：iOS 的 CNA 小窗口对 302 处理不稳定，
直接给它 200 + HTML 最可靠（它靠响应体里有没有 `Success` 字样判断是否能上网）。

注意事项（这几条都是实机踩出来的，改代码前务必看）：

- **必须设 `config.uri_match_fn = httpd_uri_match_wildcard`**
  IDF v6.1 的 `HTTPD_DEFAULT_CONFIG()` 里这一项是 `NULL`，走精确字符串比较，
  `'*'` 只是普通字符，所有通配路由静默失效。失效时的现象是日志刷
  `httpd_uri: URI '/generate_204_xxx' not found` + 404，手机一直不弹配网页
- **探测路径必须带 `*` 前缀通配**
  实测 Android 请求的是 `/generate_204_15267541384063288033`（随机数字后缀）
  和 `/generate_204_<uuid>`，只注册 `/generate_204` 精确匹配抓不到
- **通配 `*` 必须最后注册**
  `httpd_find_uri_handler()` 按**注册顺序**返回第一个匹配（不是"最长匹配"），
  而且注册过 `*` 之后就再也注册不了新 handler（内部查重时 `*` 匹配一切，
  直接返回 `ESP_ERR_HTTPD_HANDLER_EXISTS`）
- **配网期 WiFi 必须是 `WIFI_MODE_APSTA`，不能用 `WIFI_MODE_AP`**
  纯 AP 模式下 `esp_wifi_scan_start()` 直接返回 `ESP_FAIL`，"扫描附近 WiFi"
  功能不可用。STA 接口只用于扫描，不配 SSID 也不联网
- 重定向 `Location` 必须是**绝对地址**（`http://192.168.4.1/`）。相对路径会被
  Android 拼到 `connectivitycheck.gstatic.com` 上，导致打不开

## Web 使用

1. 上电，首次进入 AP 配网模式：热点 `Watchdog-AP` / 密码 `12345678`
2. 连上热点后自动弹出配网页（或手动打开 `http://192.168.4.1`），填写 WiFi
3. 设备重启联网后，访问其 IP 进入控制台（默认账号 `admin / admin123`）

### 界面概览

- **服务器状态**：脉冲呼吸 LED（绿=正常 / 橙=警告 / 红=宕机 / 灰=USB 断开），实时显示 USB 连接、心跳间隔、固件版本与构建日期（页头）、真实运行时长（右上角）
- **心跳统计**：心跳数 / 响应数 / 超时数 / 服务器已重启次数
- **固件更新 (OTA)**：拖拽或点击选择 `.bin`，显示文件名、大小、**固件版本**（按 `esp_app_desc` 结构精确解析上传镜像的版本号），带进度条与状态标签，刷写成功后自动重启
- **看门狗参数**：心跳间隔、超时时间（下限 60s）
- **登录凭据**：修改用户名 / 密码（需验证当前密码）
- **服务器电源控制**：开机 / 强制关机 / 重启 / 重置网络
- **实时事件日志**：每 4 秒刷新，按级别着色

## 认证 / 凭据

- 默认：`admin / admin123`（`main/nvs_storage.h` 的 `DEFAULT_WEB_USERNAME/PASSWORD`）
- 控制台「登录凭据」可改用户名和密码（需填当前密码校验，新密码 ≥ 4 位）
- 改密后自动失效登录态，强制重新登录
- **重置网络（Web 按钮 / 长按物理按钮）会同时把账号密码恢复为默认值**

## 心跳 / 超时参数

- 默认：每 **60 秒**发一次 `ZAIMA`，连续 **10 分钟（600 秒）**无任何响应即判定宕机 → GPIO 复位
- 控制台「看门狗参数」可调整（间隔 ≥1s，超时 ≥60s），保存至 NVS，重启保持

## OTA 固件更新

**上传方式**：前端以 `application/octet-stream` 二进制流 `POST /api/firmware`

**后端校验链**（任一失败即拒绝并回滚状态）：

1. `Content-Length` 必须存在（用于容量预校验，缺失返回 411）
2. 并发检查：已有升级进行中返回 409
3. 分区容量：固件大小 ≤ 当前 OTA 槽大小（**由分区实际大小动态判定**，超限返回 413）
4. 镜像头 magic 校验：首块必须匹配 `0xE9` 或 `0xabcdabcd`（非法返回 415）
5. 写入总数与声明大小一致性

**流程**：选择文件 → 校验 → 流式写入 OTA 分区 → `esp_ota_set_boot_partition` → 延迟 2.5s 重启 → 前端轮询 `/api/ota/status` 显示进度

**回滚**：新固件启动后 `ota_update_init()` 标记当前分区有效；若启动反复失败，bootloader 自动切回上一分区

**注意事项**

- 升级期间**务必保持供电稳定**，断电可能导致需重新烧录
- 固件大小上限：**1984 KB / OTA 槽**（4MB 分区方案，当前实际占用约 47%）
- 上传的必须是**纯 app 镜像**（`build/xxx.bin`），**不能**是 `merged.bin`（含 bootloader 的合并镜像）
- 若设备无法启动（罕见），需通过串口重新烧录

### 关于固件体积

`main/CMakeLists.txt` **刻意不依赖 `esp_https_ota`**：本项目 OTA 走本地 HTTP
（`esp_http_server` + `esp_ota_ops`），不需要 HTTPS 客户端。引入 `esp_https_ota`
会连带拖入 mbedTLS/esp-tls，白占几百 KB。请勿加回该依赖。

当前 4MB 方案（1984KB/槽）余量充足；若仍想瘦身，可尝试 `idf.py menuconfig` →
`CONFIG_COMPILER_OPTIMIZATION_SIZE=y`（最有效）、关闭
`CONFIG_ESP_ERR_TO_NAME_LOOKUP`、调低 log 级别。

## Web 页面渲染机制（重要）

页面模板**不使用 `printf` 的 `%s` / `%lu` 注入**，而是用 `{{KEY}}` 占位符 +
`ph_replace()` 字符串替换（`web_server.c`）。

**原因**：HTML/CSS 中大量出现百分号 —— `width:100%`、`translateX(-50%)`、
`radial-gradient(... 50% ...)`、`@keyframes {0%{...}}`。若把这些 HTML 交给
`snprintf` 当格式串，编译器会报

```
error: unknown conversion type character ',' in format [-Werror=format]
```

即使编译侥幸通过，运行时也会因格式符错乱导致**页面乱码甚至内存越界**。
改用占位符后，HTML 里的 `%` 就是普通字符，从根本上杜绝此类问题。

模板中的占位符（`main/dashboard_html.c`）：

```
{{VERSION}} {{BUILD_DATE}} {{LED_CLASS}} {{STATE_TEXT}} {{USB_STATE}}
{{INTERVAL}} {{TIMEOUT}} {{PARTITION}}
{{HB_COUNT}} {{RESP_COUNT}} {{TIMEOUT_COUNT}} {{REBOOT_COUNT}}
```

> 维护提示：改动 CSS 时可以放心写 `100%`、`50%`，**不需要**写成 `100%%`。
> 只有新增"动态注入值"时才需要加 `{{NEW_KEY}}` 并在 `web_server.c` 的
> `ph[]` 表里补一行。

配网页（`wizard_html.c`）无动态值，直接返回静态字符串，同样不经 `snprintf`。

## 依赖的 IDF API（v5.0+ 变更点）

本项目踩过的坑，改动代码时请留意：

| 已废弃 / 不存在 | 正确写法 | 所在组件 |
|---|---|---|
| `esp_ota_get_app_description()` | **`esp_app_get_description()`** | `esp_app_format` |
| `esp_ota_mark_app_valid_and_schedule_reboot()` | **`esp_ota_mark_app_valid_cancel_rollback()`** | `app_update` |
| `ESP_ERR_INVALID_IMAGE`（**根本不存在**） | `ESP_ERR_OTA_VALIDATE_FAILED` | `app_update` |

**回滚确认的正确姿势**（`ota_update.c` 中已实现）：

```c
esp_ota_img_states_t state;
if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
    state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();   // 自检通过, 取消回滚
}
```

只在 `PENDING_VERIFY`（新固件首次启动待确认）时标记，普通启动无需处理。
若自检失败则调 `esp_ota_mark_app_invalid_rollback_and_reboot()`。

> 另：已开启 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，若新固件来不及确认就崩溃，
> bootloader 下次启动会自动切回上一分区。

## 运行期日志排查

| 日志 | 含义 | 处理 |
|---|---|---|
| `E task_wdt: esp_task_wdt_init: TWDT already initialized` | ~~已消失~~ 旧版本会看到：自己调 `esp_task_wdt_init()` 与 IDF 启动时的自动初始化冲突 | 已修：**代码不再自行 init**，直接复用 IDF 初始化好的 TWDT，日志已无此条 |
| `E task_wdt: esp_task_wdt_reset: task not found`（**每 100ms 刷屏**） | 任务没 `esp_task_wdt_add()` 就调 `reset()` | 已修：在 `system_task` 里先注册再喂狗 |
| `Error: ClearCommError failed (设备不识别此命令)` | Windows 下 USB Serial/JTAG 重新枚举导致 `idf.py monitor` 掉线 | 见下 |
| `W NVS: Failed to read SSID: ESP_ERR_NVS_NOT_FOUND` | 首次启动没有保存的 WiFi 配置 | 正常，自动进入配网模式 |
| Web 日志面板整列显示 `undefined` | 后端 JSON 字段名与前端读取的不一致（曾后端输出 `msg`、前端读 `message`） | 已修：统一为 `message`，前端并兼容旧字段 |

### 关于 `ClearCommError failed`

应用进入 RUNNING 状态时会调 `usb_serial_jtag_driver_install()` 接管 USB，
导致 USB 重新枚举 → PC 上的 COM 口短暂消失 → monitor 报此错误并等待重连。

**这是预期行为，不是故障。** 实际部署时 USB 插在 Linux 服务器上（用于心跳），
并不接 PC，不会有这个问题。调试时如需长时间看日志，可在进入 RUNNING 前观察，
或改用串口日志输出。

### Task WDT 使用约定

本项目用 Task WDT 做"固件卡死自动重启"兜底（超时 30s）。**所有可能阻塞超过
30s 的地方都必须喂狗**，否则会被判定卡死并 panic 重启：

- `system_task` 的 RUNNING 循环 → 已喂
- `smart_config_start()` 的配网等待（最长 5 分钟）→ 已喂
- `SERVER_DOWN` 状态等待服务器重启 → 已拆成 1s 分片并喂狗

规则：**先 `esp_task_wdt_add(NULL)` 注册，才能 `esp_task_wdt_reset()`。**
注意不要在 `app_main()` 里 `add(NULL)` —— 那注册的是 main 任务，而 main 任务在
`app_main()` 返回后即被删除，会留下悬空注册项。

**代码不调用 `esp_task_wdt_init()`**，完全依赖 IDF 在启动阶段的自动初始化
（由 `CONFIG_ESP_TASK_WDT=y` 触发）。这样避免了启动日志里那条
`E task_wdt: TWDT already initialized`。

> ⚠ 因此 `sdkconfig` 里的 `CONFIG_ESP_TASK_WDT=y` / `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30`
> **不能关**。若关掉，`esp_task_wdt_add()` 会失败，兜底看门狗失效
> （代码会静默降级：标志位为 false，喂狗跳过，不报错）。

## 实测体积（基于实机 build.log）

```
segments 合计 : 955,232 字节 (932.8 KB)
ota 槽容量    : 2,031,616 字节 (1984 KB)  ← 4MB 分区方案（当前默认）
填充率        : 47.0%   剩余约 1051 KB
```

对比 2MB 方案（960 KB/槽）的 **97.2%** —— 只剩 27 KB，加任何功能就会爆，
OTA 会失败（`ESP_ERR_INVALID_SIZE`）或编译期报
`app partition is too small for binary`。

这也是本项目默认改用 4MB 的原因：你的芯片实测就是 4MB
（启动日志里有 `Detected size(4096k) larger than ... (2048k)`），
之前按 2MB 配置纯属浪费。

**现在开箱即用，无需任何手工切换。**

## 内存与栈使用约定（重要）

ESP32-C3 只有约 226KB 堆、任务栈按字节计，**大缓冲一律走堆，不要放栈上**。

### 已踩过的坑：`/api/logs` 栈溢出

`handler_logs` 曾这样写：

```c
log_entry_t ram[64];   // 140 字节 × 64 = 8.9KB
log_entry_t nvs[32];   // 140 字节 × 32 = 4.5KB   → 合计 13.4KB
```

而 httpd 任务栈只有 8KB，一进函数就撑爆：

```
Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
Detected in task "httpd"
```

现已改为 `calloc()` 堆分配，并把 httpd 栈放大到 **12288**。

### 另一个隐蔽 bug：`snprintf` 长度下溢

```c
p += snprintf(p, cap - (p - json), ...);   // ⚠ 危险
```

`p` 越过 `json + cap` 后，`cap - (p - json)` 是 **无符号数**，会下溢成
一个巨大的值，`snprintf` 随即疯狂越界写 —— 比栈溢出更难查。

现用 `JSON_LEFT()` 宏做边界保护，并在剩余空间不足 256 字节时停止拼接、
正确收尾 `]}`（宁可截断，绝不越界）。

### 约定

- 任何超过 **256 字节**的局部缓冲 → 用 `malloc/calloc`，并配对 `free`
- 新增 httpd handler 若需大缓冲，记得同步考虑 `config.stack_size`
- JSON/HTML 拼接一律用「剩余空间计算 + 返回值检查」，不要裸写长度
- **拼 JSON 时必须转义**：日志内容含 WiFi SSID 等用户输入，若含 `"` 或 `\`
  会生成非法 JSON（前端 `r.json()` 抛异常、日志面板空白）；含 `<` `>` 还会
  破坏 `innerHTML` 渲染。本项目用 `json_escape()` 统一处理，
  新增任何动态字符串进 JSON 都要走它。

### /api/logs 的内存取舍

单次请求约 **25 KB**（32+32 条日志 ≈ 8.8KB + JSON 缓冲 16KB）。
dashboard 页面另需约 20KB 渲染模板。两者并发时峰值约 45KB ——
在空闲堆 80–110KB 的 ESP32-C3 上安全，但**不要再把日志条数调回去**（曾为 64+32 条约 33KB）。

## 验证状态

- 12 个源文件通过 ESP-IDF v6.1 语法检查
- 模板文件用真实 `gcc -Werror=format` 编译通过（确认 CSS 中的 `%` 安全）
- 全项目 `printf` 格式串经脚本扫描：`%` 均为合法转换符，无 `-Wformat` 风险
- 占位符与后端替换表一一对应（模板 15 处、12 个唯一 key，重复项全部覆盖）
- 分区表经校验：4MB 默认配置占用 4096 KB / Flash 4MB ✓，2MB 备选亦验证通过
- 根 `CMakeLists.txt` 内置分区表 vs Flash 大小一致性校验（configure 阶段拦截）
- HTML 标签平衡、JavaScript 通过 `node --check`、无未声明变量
- 16 条 API/页面路由全部注册成功
