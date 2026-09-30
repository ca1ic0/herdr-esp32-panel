# Herdr ESP32 决策面板

一台通过 Wi-Fi 连接 Herdr 主机的小屏终端：首次用二维码配网，平时以四宫格查看 agent 状态，进入详情查看请求，再明确地允许、拒绝或继续。固件使用 ESP-IDF。

## 先读设计文档

1. [产品与程序逻辑](docs/PRODUCT_LOGIC.md)：状态语义、完整用户路径、不同 agent 的操作边界与 HTTP 网关契约。
2. [系统架构](docs/ARCHITECTURE.md)：现有代码问题、主机/设备模块边界、任务与消息流、内存预算及迁移顺序。
3. [UI 设计](docs/UI_DESIGN.md)：480×480 四宫格/详情/确认/配网/设置的布局、文案、手势与动画。
4. [开发顺序与验收](docs/ACCEPTANCE.md)：按阶段实施、测试样本、真机用例，以及当前代码与目标的差距。

**当前代码是早期原型，尚未实现上述决策网关和安全的审批动作。** 旧版的三个按钮直接向 pane 输入字面量 `allow`、`deny`、`continue` 并回车，不能可靠处理 Claude Code、OpenCode 或 Pi 的交互提示。没有网关语义适配器前，请只把它当实验 UI，不把审批按钮作为可用产品功能。

## 硬件与参考材料

当前代码面向 [Waveshare ESP32-C6-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16)，480×480 触摸 AMOLED。需求中“2.13 寸 OLED”与此不一致；实际板卡需要确认后再固定 UI 尺寸。厂商资料中的 CO5300/CST9220 与仓库驱动注释中的 SH8601/CST9217 也需实机核对。

仓库目前**没有** `refer/` 目录。当前可读的 REST 参考实现位于相邻目录 `/Users/calico/code/herdr-restful`，它提供 Herdr socket 的 FastAPI 包装层和验证用网页示例；该目录不属于本仓库。实现时按实际版本核对接口，产品文档中把现有接口与拟新增的 `/api/v1/panel` 接口分开列出。Herdr 官方语义参考 [Agents](https://herdr.dev/docs/agents/) 与 [Agent automation](https://herdr.dev/docs/agent-automation/)。

## 当前工程结构

```text
main/                 ESP-IDF 应用、Wi-Fi/SoftAP 配网、HTTP 客户端、LVGL 原型
components/           板级显示/触摸/电源与 LVGL 适配
docs/                 目标规格与验收清单
sdkconfig.defaults    ESP32-C6 与 LVGL 默认构建配置
partitions.csv        16 MB Flash 分区
```

现有配网页提供手动「扫描」按钮，扫描结果可点选以填写 Wi-Fi SSID；目标配网流程见产品逻辑文档。

## 编译原型

工程清单要求 ESP-IDF ≥ 5.0；当前项目维护环境为 ESP-IDF 6.1。构建命令：

```bash
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

首次配网时设备应进入 SoftAP，并显示 Wi-Fi 二维码；手机连接后可访问 `http://192.168.4.1`。现有固件仍需按 [验收清单](docs/ACCEPTANCE.md) 改造与真机验证，编译成功不表示目标产品已完成。
