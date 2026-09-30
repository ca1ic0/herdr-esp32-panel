# Herdr Panel — ESP32-C6 触摸 AMOLED 会话面板

基于 **Waveshare ESP32-C6-Touch-AMOLED-2.16** 开发板的 Herdr 会话监控面板固件。

面板是纯前端设备：通过 WiFi 连接 PC 上运行的
[herdr-restful](refer/herdr-restful/) 后端（FastAPI），轮询会话状态并渲染成
网格，同时允许对单个会话发送 `allow` / `deny` / `continue` 等按键指令。

```
┌──────────────────────────────────────────┐
│ ┌─────────────┐  ┌─────────────┐         │
│ │ ● claude    │  │ ○ codex     │         │
│ │ WORKING     │  │ IDLE        │         │  首页：纯 2×2 网格
│ │ ~/proj/api  │  │ ~/proj/web  │         │  每页 4 个会话
│ │ w1:p1       │  │ w1:p2       │         │  左右滑动翻页
│ └─────────────┘  └─────────────┘         │
│ ┌─────────────┐  ┌─────────────┐         │
│ │ ◉ codex     │  │ ● opencode  │         │
│ │ BLOCKED     │  │ DONE        │         │
│ └─────────────┘  └─────────────┘         │
│ 192.168.1.5 · 4        ● ● ○      [⚙]    │  底栏：连接状态·页点·设置
└──────────────────────────────────────────┘

点按卡片 → 详情页：输出预览 + [✓] [✗] [→] 三个按钮
```

## 首次配网（无需电脑烧配置）

设备未配置网络时自动进入**配网模式**：

1. 屏幕显示二维码（含热点信息 `HerdrPanel-XXXX`）
2. 手机扫码 → 自动连接设备热点
3. 手机自动弹出配网页（captive portal，也可手动访问 `http://192.168.4.1`）
4. 网页里填写 **WiFi SSID / 密码 / 后端地址 / 端口** → 保存
5. 设备自动重启并连接 WiFi

配网页还提供「扫描周边 WiFi」下拉框，点选即填 SSID。

之后随时可在 **⚙ Settings** 里通过触摸屏 + 虚拟键盘修改：

- **WiFi network** — SSID / 密码
- **Backend server** — 后端 IP 或主机名 / 端口
- **Reset network** — 清空 WiFi 配置，回到配网模式

配置保存在 NVS，断电不丢失；`sdkconfig` 中的同名项仅作为出厂默认值。

## 硬件

| 项目 | 说明 |
| --- | --- |
| 开发板 | [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://www.waveshare.net/wiki/ESP32-C6-Touch-AMOLED-2.16) |
| 屏幕 | 2.16" AMOLED 480×480，SH8601，QSPI（SCLK=GPIO0, D0-D3=GPIO1-4, CS=GPIO15） |
| 触摸 | CST9217 电容触摸（I2C：SCL=GPIO7, SDA=GPIO8, RST=GPIO11, INT=GPIO5） |
| 电源 | AXP2101 PMIC（I2C 0x34），ALDO3 控制屏幕供电 |
| Flash | 16MB（分区表：factory 6M + storage 3M） |

引脚定义见 [main/user_config.h](main/user_config.h)。
板级驱动组件（`components/port_bsp`、`components/pmicpower`、`components/app_bsp`）
来自厂商示例 `refer/ESP32-C6-Touch-AMOLED-2.16/02_Example/ESP-IDF-v5.5.3/09_LVGL_V9_Test`。


## 软件架构

```
main/
├── app_main.cpp        入口：板级初始化 → WiFi/配网 → LVGL → 网络任务
├── app_config.c        NVS 配置存储（WiFi + 后端地址），Kconfig 为默认值
├── provisioning.c      配网模式：SoftAP + DNS 劫持 + 配置网页 + WiFi 扫描
├── wifi_connect.c      WiFi STA 连接（自动重连）
├── herdr_client.c      HTTP 客户端：轮询 /api/v1/panes、发送输入、读输出
├── herdr_model.h       会话数据模型（状态枚举/颜色映射）
├── panel_state.c       网络任务 ↔ LVGL 之间的共享状态与消息队列
├── ui_panel.c          LVGL 界面（首页/详情/设置/配网）
├── ui_common.h         配色/字体/控件工厂
└── Kconfig.projbuild   出厂默认配置项
```

数据流：

- `net_task`（`app_main.cpp`）每 3 秒调用 `herdr_fetch_sessions()` 拉取
  `GET /api/v1/panes`，写入共享快照（`panel_state.c`）。
- LVGL 定时器（200ms）读取共享快照刷新界面。
- 点击 Allow/Deny/Continue 时，UI 把请求投入消息队列，由 `net_task` 执行
  `POST /api/v1/panes/{id}/input/text`，避免阻塞 LVGL 任务。

### 使用的后端 API

| 用途 | 端点 |
| --- | --- |
| 拉取会话列表 | `GET /api/v1/panes` |
| 读取会话输出 | `GET /api/v1/panes/{id}/output?source=recent&lines=12` |
| 发送按键指令 | `POST /api/v1/panes/{id}/input/text`，body `{"text":"...","submit":true}` |

herdr 没有专门的审批端点；`allow`/`deny`/`continue` 是通过向会话所在 pane
**输入文本并回车** 实现的（与示例 dashboard 的 sendText 一致）。默认发送的
文本就是字面量 `allow`/`deny`/`continue`，可在 menuconfig 中改成适配你的
agent CLI 审批对话框的形式（如 `1`/`2`/`y`/`n`）。

## 配置

优先级：NVS（运行时保存）> `sdkconfig` 默认值。

出厂默认值在 `idf.py menuconfig` → **Herdr Panel Configuration** 或
`sdkconfig.defaults` 中设置：

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `HERDR_WIFI_SSID` | `YOUR_SSID` | WiFi 名称（默认值，NVS 覆盖） |
| `HERDR_WIFI_PASSWORD` | `YOUR_PASSWORD` | WiFi 密码（默认值） |
| `HERDR_BACKEND_HOST` | `192.168.1.100` | 后端 IP/主机名（默认值） |
| `HERDR_BACKEND_PORT` | `8080` | 后端端口（默认值） |
| `HERDR_POLL_INTERVAL_MS` | `3000` | 会话轮询间隔 |
| `HERDR_ACTION_ALLOW_TEXT` | `allow` | ✓ 按钮发送的文本 |
| `HERDR_ACTION_DENY_TEXT` | `deny` | ✗ 按钮发送的文本 |
| `HERDR_ACTION_CONTINUE_TEXT` | `continue` | → 按钮发送的文本 |

## 构建与烧录

```bash
idf.py set-target esp32c6   # 首次
idf.py menuconfig           # 配置 WiFi / 后端地址
idf.py build
idf.py -p PORT flash monitor
```

在 VS Code 中使用 ESP-IDF 扩展：`Ctrl+Shift+P` → **ESP-IDF: Build** /
**ESP-IDF: Flash** 即可。

## 后端准备

```bash
cd refer/herdr-restful/backend
uv venv --python 3.12 .venv
uv pip install -e ".[dev]"
.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8080
```

注意监听 `0.0.0.0`（而非默认的 `127.0.0.1`），否则开发板无法从局域网访问。

## 界面交互

- **首页**：纯 2×2 网格，每格一个 herdr 会话，显示状态圆点、名称、状态
  （WORKING 黄 / BLOCKED 红 / DONE 绿 / IDLE 灰）、工作目录和 pane id。
  **左右滑动翻页**；底栏显示连接状态、页点和 ⚙ 设置按钮。
- **详情页**：点按卡片进入。显示会话内容（输出预览，3 秒自动刷新），
  底部三个操作按钮：
  - `✓`（绿）— allow
  - `✗`（红）— deny
  - `→`（蓝）— continue
- **设置**（⚙）：WiFi network / Backend server 二级表单，LVGL 虚拟键盘
  输入；Reset network 清空 WiFi 回到配网模式。保存后自动重启生效。
- **配网模式**：未配置网络时自动进入，屏幕显示二维码 → 手机扫码连热点 →
  弹出网页填 WiFi + 后端地址。

## 目录结构

```
hello_world/（项目根，工程名 herdr_panel）
├── CMakeLists.txt
├── partitions.csv          16MB flash 分区表
├── sdkconfig.defaults      出厂默认配置
├── main/                   应用代码（见上文）
├── components/
│   ├── port_bsp/           I2C / SH8601 显示 / CST9217 触摸驱动
│   ├── pmicpower/          AXP2101 电源管理（XPowersLib）
│   └── app_bsp/            LVGL 显示/触摸移植层（esp_lvgl_adapter）
└── refer/                  参考资料（厂商示例 + herdr-restful 源码）
```
