# Herdr ESP32 决策面板

一台通过 Wi-Fi 连接 Herdr 主机 panel 网关的小屏终端：首次用 WPA2 热点 + 二维码配网，平时以四宫格查看 agent 状态，进入详情查看待处理请求，再明确地允许、拒绝或继续。固件使用 ESP-IDF。

## 设计文档

实现前请先读：

1. [产品与程序逻辑](docs/PRODUCT_LOGIC.md)：状态语义、用户路径、动作状态机、`/api/v1/panel` 契约。
2. [系统架构](docs/ARCHITECTURE.md)：主机/设备模块边界、任务与消息流、内存预算、实施切片。
3. [UI 设计](docs/UI_DESIGN.md)：480×480 四宫格/详情/确认/配网/设置的布局、文案、手势。
4. [开发顺序与验收](docs/ACCEPTANCE.md)：分阶段交付、接口样本、真机用例。

**固件不向终端发字面量 `allow`/`deny`/`continue`。** 语义动作经主机 panel 网关解析后映射到各 CLI 的真实按键。没有网关适配器时按钮禁用并提示“需要在主机处理”。

## 硬件

面向 [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16)，480×480 触摸 AMOLED。厂商资料中的 CO5300/CST9220 与驱动注释中的 SH8601/CST9217 需实机核对。

## 工程结构

```text
main/
  app_main.cpp           启动与生命周期
  app_config.c/h         NVS 唯一属主；Wi-Fi + 网关地址 + 设备令牌
  wifi_connect.c/h       STA 连接与自动重连
  provisioning.c/h       WPA2 SoftAP + 配网页（含网关令牌字段）
  panel_model.h          有界数据结构与三层状态
  panel_store.c/h        快照 store + 有界队列（action_q/control_q/ui_evt_q）
  panel_api_client.c/h   唯一 HTTP 客户端，对接 /api/v1/panel
  panel_worker.c/h       请求调度、轮询、动作状态机
  ui_panel.c/h           LVGL 页面（HOM/DET/CNF/RST/SET/PRV）
  ui_common.h            色板与字体
components/              板级显示/触摸/电源与 LVGL 适配
refer/herdr-restful/     主机 REST + panel 网关参考实现
docs/                    目标规格与验收清单
```

## 主机 panel 网关

设备对接的是网关契约，不是原始 Herdr REST：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/v1/panel/overview` | 最多 24 条 agent 卡 + `total_count` |
| GET | `/api/v1/panel/agents/{terminal_id}` | 详情 + pending 卡 + `context_token` |
| POST | `/api/v1/panel/agents/{terminal_id}/actions` | 语义动作，`request_id` 去重 |

实现位于 `refer/herdr-restful/backend/app/panel/`。鉴权使用设备专属 Bearer 令牌；动作路径含现场复验、终端锁、at-most-once 去重。

## 编译

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

要求 ESP-IDF ≥ 5.x（维护环境 6.1），LVGL 9.x。

## 配网

1. 首次开机或选择“重新配网”后进入 WPA2 SoftAP `HerdrPanel-XXXX`。
2. 屏幕显示热点 SSID、临时密码与 Wi-Fi 入网二维码（二维码只含热点凭据）。
3. 手机连接热点后打开 `http://192.168.4.1`，填写家用 Wi-Fi、网关地址/端口和**设备网关令牌**。
4. 保存后设备重启并尝试连接；保存成功不等于连通，屏幕会区分状态。

网关令牌需先在主机侧生成。Wi-Fi 密码与令牌不会显示在屏幕上，也不会出现在日志中。

## 阶段说明

按 [ACCEPTANCE.md](docs/ACCEPTANCE.md)，当前仓库完成的是：

- **A 硬件与文字骨架**：BSP 沿用，中文需补 CJK 字体资产（见 `ui_common.h`）。
- **B 只读面板**：四宫格、详情、连接状态、0/1/4/5/24/25 会话布局。
- **C 网关决策（主机侧）**：`/api/v1/panel` + 适配器 + 上下文令牌 + 去重。
- **D 设备动作**：确认页、发送/回读状态机、结果未知不自动重试。

E（动画/亮度/真机压力）与真实 CLI 适配器样本需在真机与实机主机上继续验收。**编译通过不等于产品完成。**
