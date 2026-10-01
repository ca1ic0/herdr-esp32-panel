# Herdr 远程决策面板：产品与程序逻辑规格

版本：v2 草案（2026-10-01）。目标读者：负责实现 ESP-IDF 固件与 HTTP 网关的开发者或 AI。本文是目标行为，不代表当前固件已经实现。运行时设置、提示音和事件协议以 [SETTINGS_AUDIO.md](SETTINGS_AUDIO.md) 为准。

## 0. 先确认事实与范围

- 本仓库当前硬件代码针对 **Waveshare ESP32-C6-Touch-AMOLED-2.16，480×480 电容触摸屏**。用户口述“2.13 寸 OLED”与仓库不一致。落实屏幕型号、分辨率、形状和触摸能力前，不能把本文坐标直接当成另一块屏幕的交付标准。当前 UI 基线见 [UI_DESIGN.md](UI_DESIGN.md)。厂商资料列出的显示/触摸芯片为 CO5300/CST9220，仓库注释/驱动却写 SH8601/CST9217；移植前以实际板卡和厂商示例实测为准。[厂商资料](https://docs.waveshare.com/ESP32-C6-Touch-AMOLED-2.16)
- 本仓库**没有** `refer/` 目录。可读的相邻工程 `/Users/calico/code/herdr-restful` 是本次采用的 REST 包装层示例，面向 Herdr 0.9.0、socket protocol 22。其 example dashboard 仅验证 API，并非本产品 UI。若以后把 `refer/` 加回仓库，先比较接口版本，不要盲目覆盖契约。
- 产品是一台远程**查看及处理** Herdr agent 的小屏终端。手机承担 Wi-Fi 与服务器地址录入；小屏负责发现待处理会话、查看摘要、明确地执行一次决策。它不创建工作区、不启动 agent、不把整个终端搬到 2 英寸屏幕上。
- 设备经 Wi-Fi 连到主机上的 `herdr-restful` HTTP 服务。Herdr 自身的 Unix socket 仅在主机本地可用。跨公网需要由部署方提供有鉴权和加密的网关/隧道；v1 不让无鉴权 REST 服务直接暴露到公网。

## 1. 名词与不可混用的状态

| 名词 | 定义 |
| --- | --- |
| pane | Herdr 的终端位置。`pane_id` 如 `w1:p2` 可因移动而变化。 |
| terminal | 底层终端实例。用 `terminal_id` 在列表刷新后稳定追踪卡片；发动作时必须重新解析当前 `pane_id`。 |
| agent/session | 正在 pane 内运行且由 Herdr 识别的编码 agent。首页一格对应一个 agent，不显示普通 shell pane。 |
| agent 状态 | Herdr 返回的 `agent_status`：`blocked`、`working`、`done`、`idle`、`unknown`。不由设备根据输出猜测或改写。 |
| 连接状态 | Wi-Fi、REST、Herdr 上游的健康状态，与 agent 状态完全独立。 |
| 待处理卡 | 网关在当前可见终端内容中识别出的**具体交互请求**，可能是审批、问题、继续确认或无法识别。`blocked` 本身不等于审批卡。 |
| 语义动作 | `allow_once`、`allow_always`、`deny`、`continue`。语义由网关映射到当前 CLI 的真实按键/输入，固件不硬编码 `allow` 等文本。 |

Herdr 官方语义：`blocked` 是识别到审批/问题 UI；`idle`、`done` 均可接受新输入，`done` 表示完成但尚未标记为已看；`unknown` 表示无法可靠分类。读取 pane 并不会将 `done` 标记已看，显式 focus 才会影响服务端 seen 状态。状态识别可能漏报或误报，必须再检查现场内容。[Herdr Agents](https://herdr.dev/docs/agents/) · [Agent automation](https://herdr.dev/docs/agent-automation/)

### 1.1 首页状态呈现

| 后端值 | 中文 | 首页图形 | 处理逻辑 |
| --- | --- | --- | --- |
| `blocked` | 待处理 | 红色实心圆 + `!` | 优先提示，打开详情后等待待处理卡；无卡时不能允许/拒绝。 |
| `working` | 运行中 | 琥珀色进度弧 | 可看输出，不提交审批。 |
| `done` | 待查看 | 蓝色勾/圆环 | 可查看结果，可在安全条件下继续。 |
| `idle` | 空闲 | 绿色空心圆 | 可在安全条件下继续。 |
| `unknown` | 状态未知 | 灰色 `?` | 只读；不可把它当完成或待批。 |

颜色是本产品视觉映射，非 Herdr 协议字段。状态总有文字和形状，不靠颜色辨认。网络断开时卡片统一加“旧数据”遮罩，不把各 agent 状态改成 `unknown`。

## 2. 用户路径与页面状态机

```text
上电
 ├─ 无有效配置 → 配网二维码 → 手机表单 → 验证保存 → 重启/重连
 └─ 有配置 → Wi-Fi 连接 → REST/Herdr 探测 → 四宫格
                                         ├─ 点卡片 → 详情 → 决策确认 → 发送中 → 回读结果
                                         ├─ 左右滑动 → 下一/上一组四格
                                         └─ 设置 → 声音/显示/会话/连接/关于/重新配网
任意页面：网络失败 → 保留最近快照 + 离线提示；恢复后刷新，不自动重发动作。
```

### 2.1 首次配网

1. NVS 无已确认配置时启动 WPA2 SoftAP，生成本次配网专用的随机密码，屏幕展示含热点 SSID/临时密码的 **Wi-Fi 入网二维码**与备用文字 `HerdrPanel-XXXX`、临时密码、`192.168.4.1`。二维码只用于连接设备热点，不含家用 Wi-Fi 密码、服务器令牌或任何后端凭据。不能从 MAC 地址推导热点密码。
2. 手机加入热点后进入配置页；captive portal 未弹出时可手动打开 `http://192.168.4.1`。表单填 Wi-Fi SSID、密码、服务器 URL（协议、主机、端口）及**必填的设备专属网关令牌**；令牌先由主机网关管理命令生成并交给设备持有人。表单要显示普通字段的当前输入、格式校验与明确错误，令牌仅掩码显示。扫描 Wi-Fi 结果必须作为可选辅助。
3. 手机提交后，先校验字段长度/URL 和非空条件，保存到 NVS。建议先连接并探测 `GET /api/v1/health`，成功才标为“已连接”；失败保留配置并提示“网络已保存，服务器未连通”，允许手机修改地址。不能把保存成功等同连接成功。
4. 配置页/屏幕都必须能进入“重新配网”。重配网仅清除网络/网关凭据，不擦除其它 NVS 数据；执行前二次确认。异常 Wi-Fi 密码不能造成无路可回，连续失败后提供配网入口。

### 2.2 列表与会话详情

- 固件默认每 3 秒拉取一次下述 panel `overview`，用户可在设备设置中选择 2/3/5/10 秒；主机网关用 Herdr `session.snapshot` 聚合 agent 与 pane。只展示正在运行的 agent；0 个时明确显示“暂无 Herdr 会话”。最多缓存 24 个，超过 24 个要显示“仅显示前 24 个”及总数，不可无提示丢弃。设备/主机的模块和数据所有权见[系统架构](ARCHITECTURE.md)。
- 默认卡片按首次加载时 `workspace_id/tab_id/pane_id` 排列，刷新只更新内容，不因状态变化自动洗牌。用户可动态改成“待处理优先”或“最近更新”，切换时明确重新排序并尽量保持当前 `terminal_id` 可见。新增卡放末尾；消失卡移除后补位。可选隐藏空闲卡，但首页同时显示隐藏数量。顶部有待处理数；点它跳到第一个可见 `blocked` 卡。
- 点卡片进入详情，标题使用 `display_agent`/agent 名称，项目使用工作目录末段（如 `api`），保留完整 `pane_id` 于信息菜单；可看最近输出，但**当前交互判断使用 `visible` 或 `detection`，不能只用历史 `recent`**。终端输出需要去 ANSI、过滤控制字符、截断长度；任何显示文本都视为不可信数据。
- 进入详情只读，不调用 `agent.focus`，避免意外改变主机上的焦点/seen。详情找不到 agent 时显示“会话已结束”，退回列表；同一 `terminal_id` 换成另一 agent 时必须清除旧待处理卡和动作。

### 2.3 允许、拒绝、继续

所有动作经过 **查看现场 → 展示对象和影响 → 用户确认 → 网关复验 → 单次提交 → 回读**。设备不得在点卡片或状态变为 `blocked` 时自动执行动作。

| 动作 | 显示条件 | 执行意义 | 常见不可用原因 |
| --- | --- | --- | --- |
| 允许一次 | `blocked` 且当前待处理卡是可识别审批，明确有“一次允许”选项 | 仅响应这一条请求 | 问题卡、未知提示、屏幕过期、CLI 不支持 |
| 始终允许 | 仅对应 CLI 当前提示提供且网关能证明选项范围时；放在二级菜单并强确认 | 可能改变本项目/会话后续审批规则 | 缺少选项范围、策略禁用、`unknown` |
| 拒绝 | `blocked` 且待处理卡提供明确拒绝选项 | 拒绝这一条请求，可按 CLI 能力附原因 | 未识别当前选项、CLI 不支持 |
| 继续 | `idle`/`done` 且网关确认 agent 仍在、可接受 prompt；或明确识别的“继续?”对话 | 发送**确认页可见**的继续提示词，或选择当前对话中的继续选项 | `working`、未知、审批中、无继续文案 |

`continue` 的默认提示词建议为“继续当前任务，并先说明下一步。”，可在临时手机配置页运行时编辑；固件确认页需显示将发送的文字，不能暗中追加新的任务。保存新文案不改变已经打开的确认页。`blocked` 的自由问答不复用“允许”按钮；可在详情显示“请在主机处理”，以后再增加问答功能。对于待批卡无法完整显示影响（命令、文件路径、权限范围等）时，按钮禁用，并引导主机处理。

失败状态：HTTP 成功只说明“输入已送到 Herdr”，**不代表 CLI 已批准或执行成功**。网关随后读取新状态/输出；若在 5 秒内未观察到提示变化，显示“已发送，结果待确认”。超时、连接中断、HTTP 5xx 的动作结果标为“结果未知”，不自动重试；用户查看新快照或主机终端后再操作。

动作状态机须按下表实现，`request_id` 自首次发送起到最终结果不变：

| 当前状态 | 事件 | 下一状态 | 必须做的事 |
| --- | --- | --- | --- |
| `ready` | 点可用动作 | `confirming` | 冻结待处理卡和确认页显示内容，继续轮询现场。 |
| `confirming` | 取消/10 秒未确认 | `ready` | 丢弃动作；不发送任何请求。 |
| `confirming` | 现场/连接变化 | `stale` | 关闭确认；提示重新查看。 |
| `confirming` | 用户确认 | `sending` | 锁定按钮；网络任务取走动作时生成一次 `request_id`，只发一次请求。 |
| `sending` | 网关 `accepted` | `delivered` | 显示“已发送”，等回读证据。 |
| `sending` | 409 `stale_context` / 422 `unsupported` | `stale` / `unsupported` | 直接显示原因，不尝试备用按键。 |
| `sending` | 401/403 或 409 `id_conflict` | `unavailable` | 显示凭据/协议错误，清除旧选项，不换 ID 重发。 |
| `sending` | 200 `uncertain` | `uncertain` | 显示“结果未知”，只允许刷新，不自动重试。 |
| `sending` | 超时/断线/5xx | `uncertain` | 显示“结果未知”，只允许刷新，不自动重试。 |
| `delivered` | 观察到适配器定义的目标提示消失/状态转移证据 | `observed` | 显示“已处理”；再刷新详情。 |
| `delivered` | 5 秒仍无明确变化 | `uncertain` | 显示“结果待确认”，不推断成功。 |

同一个确认页只能触发一次 `sending`；重复点击和快速双击都被锁住。用户再次操作必须重新打开最新详情，拿新 `context_token`。

## 3. Agent 差异及 Auto 的边界

| Agent | Herdr 状态来源 | 审批交互 | 本产品 v1 支持原则 |
| --- | --- | --- | --- |
| Claude Code | Herdr 屏幕检测；集成侧重会话身份/恢复 | 有不同权限模式，包括 `default`、`acceptEdits`、`auto` 等；具体提示的选项随版本/模式变化 | 网关适配当前可见审批；“Auto”是**主机 Claude 权限模式**，不是一次性审批按钮。v1 只显示经验证的模式信息，不在设备上盲切模式。 |
| OpenCode（若“openroute”指它） | Herdr 集成可上报生命周期状态 | `once`、`always`、`reject`；OpenCode 也有 auto 机制，取决于版本/配置 | 同一套语义动作，由 OpenCode 适配器映射当前选项；不能把 `always` 和 `auto` 合并。 |
| Pi | 安装 Herdr 集成后状态较可靠；无集成时依赖屏幕识别 | 原生 Pi 通常**没有统一的审批弹窗**，权限确认可能来自扩展 | 默认仅查看/继续；只有部署了已知扩展适配器且识别到真实审批卡，才显示允许/拒绝。 |
| 其它/未知 | 取决于 Herdr 检测与集成 | 未定义 | 只读和经确认的继续；没有适配器时不提供审批快捷键。 |

`openroute` 若指 **OpenRouter**，它提供模型 API，不是 Herdr agent 类型；操作取决于使用它的 CLI（Claude/OpenCode/Pi 等）。实现时按 `agent` 类型选择适配器，不按模型提供商选择。[OpenRouter 文档](https://openrouter.ai/docs/quickstart) · [Herdr agents](https://herdr.dev/docs/agents/) · [Herdr integrations](https://herdr.dev/docs/integrations/) · [Claude 权限模式](https://code.claude.com/docs/en/permissions) · [OpenCode 权限](https://opencode.ai/docs/permissions/) · [Pi 安全说明](https://pi.dev/docs/latest/security)

### 3.1 Auto 菜单

- 详情可以显示 `手动/自动/未知` 标签，**只有后端从该 CLI 的可信配置或接口查到时才显示**，否则显示“权限模式未知”。Herdr 的 `agent_status`、`state_labels`、终端文字均不能证明当前权限模式。
- v1 菜单中的“自动模式”是说明页，指引用户在主机 CLI 内切换。不得通过向终端发送 `/permissions`、Shift+Tab 或字面量 `auto` 来猜测切换。
- 若以后做设备切换，需增加显式的“读模式/写模式”网关能力、版本检测、影响范围、二次确认和切换后回读；若无可靠读回能力则不能提供开关。

## 4. 远程 API：已有接口与新增网关契约

### 4.1 当前 `herdr-restful` 已有接口

| 目的 | 方法/路径 | 注意 |
| --- | --- | --- |
| 健康检查 | `GET /api/v1/health` | 区分 REST 活着与 Herdr socket 可用。 |
| 会话快照 | `GET /api/v1/session/snapshot` | 可得 panes、agents、workspaces；响应可能较大。 |
| 列出 agents | `GET /api/v1/agents` | `type: agent_list`，可与 pane 元数据关联。 |
| 列出 panes | `GET /api/v1/panes` | 包含普通终端；不能直接都当 agent 卡。 |
| 读当前屏 | `GET /api/v1/agents/{pane_id}/output?source=visible` | 用于决策现场。 |
| 读短摘要 | `GET /api/v1/panes/{pane_id}/output?source=recent&lines=12` | 用于人类查看，不作为审批匹配证据。 |
| 对 agent 按键 | `POST /api/v1/agents/{pane_id}/keys`，`{"keys":["enter"]}` | 校验当前 occupant，比 pane 原始输入更合适。 |
| 对 agent 发 prompt | `POST /api/v1/agents/{pane_id}/prompt`，`{"prompt":"...","submit":true}` | 仅用于安全条件下的继续。 |

这些端点都没有“审批”语义或事务校验；当前固件所用 `POST /api/v1/panes/{id}/input/text` 发 `allow`/`deny`/`continue` 只是原始终端输入，不能作为产品验收的审批实现。上述路径以本机相邻示例工程为准，升级 Herdr/REST 后必须重新核对。

### 4.2 必须在主机网关新增的 panel API

固件不解析各 CLI 的 TUI。主机网关负责适配器、现场校验、鉴权和去重。可在 `herdr-restful` 新增 `/api/v1/panel` 路由，也可由相同契约的独立小服务实现。下面是**本产品拟新增接口，不是 Herdr 官方 API**。

`GET /api/v1/panel/overview`：返回 `schema_version: 1`、`server_id`、`snapshot_at`、连接健康（`ok|degraded`）、总数、agent 数组。每个 agent 至少含 `terminal_id`、当前 `pane_id`、`agent`、`display_name`、`workspace_label`、`cwd_tail`、原样 `herdr_status`、`revision`、`updated_at`。最多给固件 24 条，且另给 `total_count`。`server_id` 在网关重启或 Herdr 会话连续性丢失时变化；变更后固件必须清空会话缓存和全部待提交动作。

通知是**事件而非当前状态**。声音与视觉提醒通过另一个 `GET /api/v1/panel/events` 游标接口传递；设备绝不能对每次 `overview` 中的 `blocked`/`done` 自动播音。事件来源、冷启动基线、去重与免打扰规则见 [SETTINGS_AUDIO.md](SETTINGS_AUDIO.md)。

```json
{
  "schema_version": 1,
  "server_id": "host-a-boot-42",
  "snapshot_at": "2026-09-30T12:00:00Z",
  "health": "ok",
  "total_count": 1,
  "agents": [{
    "terminal_id": "term-a",
    "pane_id": "w1:p1",
    "agent": "claude",
    "display_name": "Claude Code",
    "workspace_label": "api",
    "cwd_tail": "project-api",
    "herdr_status": "blocked",
    "revision": 7,
    "updated_at": "2026-09-30T11:59:59Z"
  }]
}
```

`GET /api/v1/panel/agents/{terminal_id}`：返回最新 agent 元信息、最多 12 行净化后的输出摘要和一张 `pending` 卡。`pending` 必须包括 `kind`（`approval|question|continuation|unrecognized|none`）、完整可读摘要、影响对象、`choices`（`allow_once|allow_always|deny|continue` 的子集）、`context_token`、`expires_at`；无法证明选项时 `choices: []`。每次读现场前重新解析 `terminal_id → pane_id`，对照 agent 类型和当前可见屏。输出裁剪后不足以判断影响时标记 `unrecognized`。

```json
{
  "schema_version": 1,
  "server_id": "host-a-boot-42",
  "terminal_id": "term-a",
  "pane_id": "w1:p1",
  "agent": "claude",
  "herdr_status": "blocked",
  "output_lines": ["Run command?", "npm test"],
  "permission_mode": null,
  "pending": {
    "kind": "approval",
    "summary": "运行 npm test",
    "impact": "在 /repo/project-api 执行命令，仅本次",
    "source": "terminal_ui",
    "choices": ["allow_once", "deny"],
    "context_token": "opaque-10-second-token",
    "expires_at": "2026-09-30T12:00:10Z"
  }
}
```

`idle`/`done` 时，可把“输入新的继续提示词”表示为 `pending.kind=continuation`、`choices=["continue"]`；只有识别了终端内的继续确认时才把那个 CLI 选项映射成 `continue`。二者需有 `pending.source`：`new_prompt` 或 `terminal_ui`，执行路径不同。`permission_mode` 为 `null` 时 UI 显示“权限模式未知”，不能猜测 Auto。`summary`/`impact` 无法放进确认页完整展示时，网关须返回 `choices=[]`。

`POST /api/v1/panel/agents/{terminal_id}/actions`：请求例子：

```json
{
  "action": "allow_once",
  "context_token": "opaque-token-from-detail",
  "request_id": "random-128-bit-id"
}
```

继续新任务时另传 `prompt`，并在确认页展示同一文本。网关必须：验证设备鉴权；用 `request_id` 去重（至少保留 10 分钟）；重新解析 agent/终端；重新读当前可见屏并计算上下文；核对 agent 类型、状态、可选项和令牌；**仅在完全匹配时执行一次**。可使用 Herdr `agent.send_keys` 或 `agent.prompt`，但映射按实际 CLI 版本与待处理卡决定。不能在失败时退化成 `pane.send_text`。返回 `accepted`（输入已送出）、`observed`（后续状态已确认）、`stale_context`（409，现场改变）、`unsupported`（422）、`offline`（503）等明确结果；正文至少有 `request_id`、`result`、`message`。即使网络层重放同一 `request_id`，网关也只能返回已记录结果，不得再次发键；设备收到不确定结果时仍不自动重发。`request_id` 由固件 CSPRNG 生成 128 位随机值，在一次动作生命周期中保持不变。

推荐 `context_token` 包含服务端签名/随机句柄，绑定 `server_id`、`terminal_id`、pane 当前 occupant、agent 类型、提示内容指纹和生成时间，TTL 不超过 10 秒。主机侧处理 CLI 特定的键盘步骤；固件既不持有快捷键表，也不实现屏幕文本正则判别审批。

动作接口的 HTTP/正文映射要固定，不能靠错误字符串猜测：

| HTTP | `result` | 设备处理 |
| --- | --- | --- |
| 200 | `accepted` / `observed` | 进入送达/已观察状态；`accepted` 仍需回读。 |
| 200 | `uncertain` | 显示结果未知；不得自动重发。 |
| 401/403 | `unauthorized` | 清除可执行选项，提示更新凭据。 |
| 409 | `stale_context` | 关闭确认页，刷新当前详情；旧 token 不再可用。 |
| 409 | `id_conflict` | 同一 ID 携带不同请求体；记协议错误，不换 ID 重发。 |
| 422 | `unsupported` | 显示主机处理；不尝试其它按键。 |
| 503 | `offline` | 标记结果未知并刷新连接；不要假定请求未发送。 |
| 无响应/其它 5xx | 无可信结果 | 标记结果未知；不得自动重发。 |

网关自行生成的动作成功与错误响应都带 `schema_version`、`request_id`、`result` 和供 UI 使用的安全文案 `message`；异常追踪 ID 可另给，不向设备返回内部栈/原始终端全文。设备也必须处理代理/网络返回的非 JSON 错误。列表/详情的 401、403、畸形 JSON、协议版本不符同样须分别显示凭据/协议问题，不渲染成“暂无会话”。

适配器以 `agent` 类型和经测试的 CLI 版本为键，输出“识别到的提示类型、可选项、影响范围、对应按键序列和验证条件”。缺任一项、版本未知、提示不完整时返回 `unrecognized`。自动化用例必须保存真实 CLI 的屏幕样本与适配结果；不能仅根据 `blocked` 状态或匹配一个 `Allow` 单词启用按钮。

### 4.3 连接与数据新鲜度

- Wi-Fi、REST、Herdr socket 分别建状态。`GET /health` 成功但 Herdr 降级时显示“服务器异常”，不得当作 0 个会话。
- overview 默认每 3 秒拉取，按用户设置调整；详情元信息/短输出每 2 秒拉取；通知事件默认每 2 秒取一次；出现 `blocked` 时立即拉一次详情。列表/详情 HTTP 超时建议 2 秒，动作建议 5 秒；失败后指数退避 1/2/4/8/15 秒并限制上限；恢复后立刻全量刷新状态，但声音事件先建立新基线，不补播离线历史。避免多个大响应并行耗尽内存。
- 快照超过 6 秒标“更新延迟”，超过 10 秒所有动作禁用；网络失败直接禁用动作。列表保留最近成功数据，标注最近更新时间；重启后不从 NVS 恢复会话数据。固件用收到响应时的单调时钟计算“已过去 N 秒”，不依赖尚未同步的 RTC；令牌有效期由网关自己判定。
- `pane_id`/agent 类型/`server_id` 任一变化，立即丢弃旧待处理卡与令牌；`terminal_id` 消失或指向别的 agent 时清除整个详情。只有 `pane_id` 移动时可保留所选 `terminal_id`，但必须重新取得详情。正在确认时发生上述变化则关闭确认页并提示“现场已变化”。
- 所有 panel 读取和输入端点必须有认证。优先网关提供经过证书验证的 HTTPS + 设备专属 Bearer token，或设备与主机同处受控隧道；如首版仅 HTTP，限制在可信局域网内，不可把端口暴露到公网。主机提供令牌生成/吊销入口；NVS 保存令牌，不在 QR、屏幕、串口日志、错误提示中回显。URL/JSON 长度必须有限制并处理非 2xx 与畸形响应。

## 5. 固件任务划分（ESP-IDF）

```text
app_main
 ├─ board/power/display/touch 初始化
 ├─ NVS 配置 + Wi-Fi/SoftAP 配网
 ├─ net_task：HTTP 队列、轮询、解析、退避、动作提交、回读
 └─ LVGL task：只读快照 → 页面渲染；触摸事件 → 有界消息队列
```

- 仅 LVGL 所在线程/加锁上下文触碰 LVGL 对象；HTTP 绝不在 LVGL 回调中同步运行。
- 网络任务发布不可变快照或在互斥锁下复制结构；UI 用 `terminal_id` 选择，不用数组下标作为跨刷新身份。UI 命令携带动作上下文，网络任务取走动作时生成 `request_id`，结果事件再带回该 ID；队列满时明确提示“忙，请稍后”。
- JSON 使用有限大小缓冲/流式解析并报告截断；缺失 `schema_version` 或重复 `terminal_id` 时拒绝整份快照；单条记录的未知状态值映射为 `unknown` 并记录原值。所有外来文字限制 UTF-8 字节/字符数，显示层转义控制字符。
- 配置区分“出厂默认”和“用户保存”；默认的 `YOUR_SSID` 之类占位符不能让设备误认为已配网。Wi-Fi 断线自动重连；修改服务器地址只刷新连接，修改 Wi-Fi 可重启或重新连接。
- 设置有屏幕即时预览、NVS 保存确认和失败回滚；亮度、声音、动效、排序和轮询间隔均能运行中更改，无需刷固件或重启。音频由独立任务持有 ES8311/I2S，声音故障不阻塞 UI/网络。字段、默认值和事件契约见 [SETTINGS_AUDIO.md](SETTINGS_AUDIO.md)。
- 开发阶段至少记录匿名诊断：HTTP 状态、Herdr 状态、动作结果码、时间戳、当前页。禁止记录 Wi-Fi 密码、token、完整 agent 输出、审批对象敏感文本。

## 6. 明确不做与验收门槛

v1 不提供终端全键盘、任意命令、批量审批、自动审批规则编辑、从屏幕切换 Claude/OpenCode Auto、创建/关闭 pane。只有上述安全路径已实现并验证后，才能在小屏显示可点击的允许/拒绝按钮。任何未实现能力须在 UI 中不可用并附原因，不能放一个会发固定字符串的假按钮。

详细验收用例见 [ACCEPTANCE.md](ACCEPTANCE.md)。
