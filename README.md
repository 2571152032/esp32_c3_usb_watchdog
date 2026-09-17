# ESP32-C3 USB Watchdog

Linux 服务器硬件看门狗：USB CDC-ACM 心跳 + GPIO 硬件控制 + Web 控制台 + OTA 固件更新。

## 功能

- **USB CDC-ACM 心跳**：默认每 60 秒发一次 `ZAIMA`，10 分钟（600s）无响应触发 GPIO 复位
- **GPIO 控制**：服务器复位 / 开机 / 强制关机 / 电源状态检测（PWR_LED）/ LED / 配网按钮
- **SmartConfig 配网**：AP 热点 + Web 向导
- **Web 控制台**（深色玻璃拟态主题）：状态监控、心跳统计、参数设置、凭据管理、**固件 OTA 更新**、实时事件日志
- **Web 认证**：默认 `admin / admin123`，可修改；重置网络后恢复默认值
- **OTA 双分区**（factory + ota_0 + ota_1），支持失败自动回滚

## 目录结构

```text
esp32_c3_usb_watchdog/
├── CMakeLists.txt           # 项目注册 (PROJECT_VER = 1.2.31)
├── partitions.csv           # OTA 双分区表 (4MB Flash 版, 默认)
├── sdkconfig.defaults       # 默认配置
├── server/                  # Linux服务器端心跳回复服务
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
    ├── smart_config.c/.h    # AP 配网
    ├── wizard_html.c/.h     # 配网向导页面
    ├── gpio_control.c/.h    # GPIO 控制
    ├── nvs_storage.c/.h     # NVS (WiFi + 凭据 + 心跳参数)
    ├── event_log.c/.h       # 事件日志 (RAM + NVS)
    └── uptime.c/.h          # 运行时间 / 重启计数
```

## 构建（ESP-IDF v6.1）

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

**版本号**：根 `CMakeLists.txt` 中的 `set(PROJECT_VER "1.2.31")` 控制。发版时改这一处即可，
版本号与构建日期会自动写入镜像 `esp_app_desc_t`，Web 控制台页头显示
`固件版本 · v1.2.31 · 构建日期 Sep 16 2026`，OTA 上传页解析的也是同一字段。

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

## Web 使用

1. 上电，首次进入 AP 配网模式：热点 `ESP32-Watchdog` / 密码 `12345678`
2. 浏览器打开 `http://192.168.4.1`，填写 WiFi
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
