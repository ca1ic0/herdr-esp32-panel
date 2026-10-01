# Herdr ESP32 决策面板

一台通过 Wi-Fi 连接 Herdr 主机 panel 网关的小屏终端：首次用 WPA2 热点 + 二维码配网，平时以四宫格查看 agent 状态，进入详情查看待处理请求，再明确地允许、拒绝或继续。固件使用 ESP-IDF。

## 设计文档

实现前请先读：

1. [产品与程序逻辑](docs/PRODUCT_LOGIC.md)：状态语义、用户路径、动作状态机、`/api/v1/panel` 契约。
2. [系统架构](docs/ARCHITECTURE.md)：主机/设备模块边界、任务与消息流、内存预算、实施切片。
3. [UI 设计](docs/UI_DESIGN.md)：480×480 四宫格/详情/确认/配网/设置的布局、文案、手势。
4. [开发顺序与验收](docs/ACCEPTANCE.md)：分阶段交付、接口样本、真机用例。
5. [运行时设置与声音](docs/SETTINGS_AUDIO.md)：动态设置、Herdr 两类提示音、通知事件契约、音频硬件和验收。

**固件不向终端发字面量 `allow`/`deny`/`continue`。** 语义动作经主机 panel 网关解析后映射到各 CLI 的真实按键。没有网关适配器时按钮禁用并提示“需要在主机处理”。

## 硬件

面向 [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16)，480×480 触摸 AMOLED。厂商资料中的 CO5300/CST9220 与驱动注释中的 SH8601/CST9217 需实机核对。

该板有 ES8311 音频链路和外接扬声器焊盘。声音功能还需确认成品已连接扬声器，并核对当前板版 I2S 引脚。目标提示音使用 Herdr 官方 `request`（需要输入）和 `done`（任务完成）素材；状态快照本身不能可靠地触发一次性声音。

## 工程结构

```text
main/
  app_main.cpp           启动与生命周期
  app_config.c/h         Wi-Fi、网关、设备令牌及继续提示词 NVS 配置
  wifi_connect.c/h       STA 连接与自动重连
  provisioning.c/h       WPA2 SoftAP + 首次配网/临时编辑网页
  panel_model.h          有界数据结构与三层状态
  panel_store.c/h        快照 store + 有界队列（action_q/control_q/ui_evt_q）
  panel_api_client.c/h   唯一 HTTP 客户端，对接 /api/v1/panel
  panel_worker.c/h       请求调度、轮询、动作状态机
  panel_prefs.c/h        运行时设置、范围校验与 NVS 保存
  panel_audio.cpp/h      ES8311/I2S 播放任务及有界音效队列
  panel_power.h          后台 AXP2101 电量快照接口
  ui_panel.c/h           LVGL 页面（BOOT/HOM/DET/CNF/RST/SET/PRV）
  ui_common.h            色板与字体
components/              板级显示/触摸/电源与 LVGL 适配
docs/                    目标规格与验收清单
```

本地仓库目前没有 `refer/` 目录；主机网关实现位于相邻工程 `herdr-restful`（`backend/app/panel/`，见下节），接口版本变更需与本仓库 `panel_model.h` 同步核对。

## 主机 panel 网关

网关代码在 `herdr-restful/backend/app/panel/`：FastAPI 路由 + 读模型聚合 + Claude/OpenCode/Pi 适配器 + HMAC 上下文令牌 + SQLite `request_id` 幂等表。启动前在主机生成设备令牌：

```bash
cd herdr-restful/backend
python -m app.panel.tokens            # 生成一台设备的令牌
export HERDR_RESTFUL_PANEL_TOKENS=hp_xxx   # 逗号分隔可配多台
herdr-restful                          # 照常启动 REST 服务
```

设备对接的是网关契约，不是原始 Herdr REST：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/v1/panel/overview` | 最多 24 条 agent 卡 + `total_count` |
| GET | `/api/v1/panel/agents/{terminal_id}` | 详情 + pending 卡 + `context_token` |
| POST | `/api/v1/panel/agents/{terminal_id}/actions` | 语义动作，`request_id` 去重 |
| GET | `/api/v1/panel/events?after=...` | 请求/完成状态转移事件与游标；实现位于相邻 `herdr-restful` 仓库 |

鉴权使用设备专属 Bearer 令牌；动作路径需要现场复验、终端锁、at-most-once 去重。`/events` 由主机网关实现，面板使用 [事件契约](docs/SETTINGS_AUDIO.md) 拉取；没有 Herdr 原生通知源时，其事件来自状态转移，属于近似通知。

## 编译

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

要求 ESP-IDF ≥ 5.x（维护环境 6.1），LVGL 9.x。
`.github/workflows/firmware-build.yml` 在推送时用 ESP-IDF 6.1 构建；固件是否发声及触摸/显示兼容性仍须用实际成品验收。

## 字体

`main/fonts/` 内是预生成的 CJK 位图字体（GB2312 一级汉字 3755 字 + 常用标点，16/20/24px），由 `tools/fonts/make_cjk_fonts.sh` 用 lv_font_conv 从 Noto Sans CJK SC 光栅化而来，作为 Montserrat 的 LVGL fallback 链生效。重新生成需要 node/npx 与 `tools/fonts/NotoSansCJKsc-Regular.otf`（不随仓库分发）。字形采用 SIL Open Font License 1.1，许可证文本见 `tools/fonts/OFL.txt`。

## 配网

1. 首次开机或选择“重新配网”后进入 WPA2 SoftAP `HerdrPanel-XXXX`。
2. 屏幕显示热点 SSID、临时密码与 Wi-Fi 入网二维码（二维码只含热点凭据）。
3. 手机连接热点后打开 `http://192.168.4.1`，填写家用 Wi-Fi、网关地址/端口和**设备网关令牌**。
4. 保存后设备重启并尝试连接；保存成功不等于连通，屏幕会区分状态。

已配网时可从“连接→扫码编辑连接”或“会话→扫码编辑继续提示词”进入临时编辑热点；5 分钟后自动退出，也可在设备上取消。网页中留空的连接字段保留旧值，勾选“恢复网关默认提示词”清除设备覆盖值。编辑连接会软重启，不清除声音、显示和会话偏好。自定义继续提示词最多 160 个 UTF-8 字节，审批/终端原生继续选项仍由主机网关现场复验。

网关令牌需先在主机侧生成。Wi-Fi 密码与令牌不会显示在屏幕上，也不会出现在日志中。

## 阶段说明

按 [ACCEPTANCE.md](docs/ACCEPTANCE.md)，当前仓库可见的实现是：

- **A/B 硬件与只读面板**：BSP、CJK 字体、配网二维码、四宫格、详情与连接状态已有固件实现；显示芯片、触摸、扫码和 0/1/4/5/24/25 会话仍需实机复核。
- **C/D 网关决策与设备动作**：相邻 `herdr-restful` 已实现 overview/detail/actions/events、上下文令牌、终端锁与幂等去重；确认页和动作结果状态机已有固件实现。真实 Claude/OpenCode/Pi 审批屏及发送动作仍需端到端验证。
- **E/F 视觉与运行时设置**：开机四格动效、AXP2101 电量显示、分组设置与 NVS 持久化、临时热点编辑连接和继续提示词已落地。连接编辑目前通过软重启切换热点；设置滑杆尚未做 500 ms NVS 写入合并。
- **G 声音**：固件使用 ES8311/I2S 播放固定 Herdr `request`/`done` 素材；主机事件 API 当前按状态转移产生近似通知。扬声器焊接、引脚、音量和实际触发时机须在真机与 Herdr 主机上验收。

GitHub Actions 用 ESP-IDF 6.1 编译固件。**编译通过不等于产品完成**；目前没有成品硬件测量结果，也不能宣称事件音与 Herdr 原生通知完全同步。
