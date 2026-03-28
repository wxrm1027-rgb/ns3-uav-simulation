# 异构无人机集群网络管理仿真 — 自主可视化平台设计方案

## 一、技术选型建议

| 方案 | 技术栈 | 与 NS-3 联动 | 优点 | 缺点 | 推荐度 |
|------|--------|--------------|------|------|--------|
| **A. Web 前端** | HTML5 + JavaScript + ECharts + Canvas/SVG | 离线：读 JSON/CSV/日志；实时：WebSocket + 桥接进程 | 无需重编 NS-3、跨平台、易部署、与现有 NetAnim XML/日志兼容 | 需浏览器；实时需额外桥接 | ★★★★★ |
| **B. Python 桌面** | Python 3 + PySide6 + pyqtgraph/NetworkX + Matplotlib | 离线：读文件；实时：Socket 直连或 tail 日志 | 与 NS-3 无编译耦合、开发快、可嵌 C++ 扩展 | 依赖 Python 环境、打包略重 | ★★★★ |
| **C. Qt/C++** | Qt5/6 + QGraphicsView + QChart | 实时：NS-3 内嵌或同进程 Socket | 与 NS-3 同语言、性能最好、可集成进 ns-3 | 开发周期长、需维护 C++/Qt 构建 | ★★★ |

**推荐：以 Web 方案为主**，理由：

1. **零侵入 NS-3 可执行文件**：沿用现有 `NmsLog` 写文件 + 归档目录，仅需约定日志/拓扑格式（或增加轻量 JSON 导出）。
2. **双模式**：离线直接读 `simulation_results/<时间戳>/log/`、`performance/`、`visualization/` 下文件；实时可通过小型桥接（Python/Node 脚本 tail 日志或接收 Socket）推 WebSocket。
3. **界面迭代快**：改 HTML/JS 即可，无需重新编译 ns-3。
4. **风格统一**：ECharts 做统计图，Canvas/SVG 做拓扑，工业风配色易实现。

---

## 二、整体架构设计

### 2.1 模块划分

```
┌─────────────────────────────────────────────────────────────────┐
│                    可视化平台（Web 单页应用）                      │
├──────────────┬──────────────┬──────────────┬─────────────────────┤
│  拓扑模块    │  监控模块    │  回放模块    │  统计图表模块        │
│  - 节点/链路 │  - 节点状态  │  - 时间轴    │  - 吞吐/时延/丢包   │
│  - 拖拽缩放  │  - 筛选器    │  - 事件列表  │  - 能量/Score 趋势   │
│  - 子网着色  │  - 实时刷新  │  - 暂停/继续 │  - 导出 CSV          │
└──────────────┴──────────────┴──────────────┴─────────────────────┘
        │                │                │                │
        └────────────────┴────────────────┴────────────────┘
                                    │
                    ┌───────────────┴───────────────┐
                    ▼                               ▼
            ┌───────────────┐               ┌───────────────┐
            │  离线数据源   │               │  实时数据源   │
            │  - topology   │               │  - WebSocket  │
            │  - events     │               │  - 桥接进程   │
            │  - flowmon    │               │  (tail/转发)  │
            └───────┬───────┘               └───────┬───────┘
                    │                               │
                    ▼                               ▼
            ┌───────────────────────────────────────────────┐
            │  NS-3 仿真                                     │
            │  - 现有 NmsLog → log/nms-framework-log_*.txt   │
            │  - 可选：JSON 行 或 Socket 推送                 │
            │  - FlowMonitor → performance/flowmon_stats_*   │
            └───────────────────────────────────────────────┘
```

### 2.2 与 NS-3 的交互流程

**离线模式（当前即可用）：**

1. 运行仿真 → 生成 `simulation_results/<时间戳>/` 下 `log/`、`performance/`、`visualization/`。
2. 可视化平台打开「加载本次仿真」→ 选择该时间戳目录（或其中导出的 `topology.json` + `events.json`）。
3. 拓扑从 NetAnim XML 或专用 `topology.json` 解析；事件从 `nms-framework-log_*.txt` 解析（或由转换脚本生成 `events.json`）；统计从 `flowmon_stats_*.xml` / `.txt` 读取。
4. 时间轴按事件时间戳回放，图表展示 flowmon 统计。

**实时模式（可选扩展）：**

1. NS-3 侧：在 `NmsLog` 或关键事件处增加 UDP/TCP 发送，将「时间戳、节点ID、事件类型、详情」发往本机某端口；或保持写文件，由桥接进程 tail 该文件。
2. 桥接进程（Python/Node）：监听端口或 tail 日志 → 解析为结构化事件 → WebSocket 推送给浏览器。
3. 前端已连接 WebSocket → 收到事件即更新拓扑状态、监控面板、事件列表，并驱动时间轴。

### 2.3 数据格式约定

**拓扑（topology.json）：**

```json
{
  "nodes": [
    { "id": 0, "label": "GMC", "role": "gmc", "subnet": "gmc", "x": 0, "y": 0 },
    { "id": 4, "label": "LTE-eNB", "role": "spn", "subnet": "lte", "x": 50, "y": 50 },
    { "id": 5, "label": "LTE-UE", "role": "ue", "subnet": "lte", "x": 60, "y": 55 }
  ],
  "links": [
    { "from": 0, "to": 4, "type": "backhaul" },
    { "from": 5, "to": 4, "type": "data" }
  ]
}
```

**事件流（events.json 或 JSON Lines）：**

```json
{ "t": 1.234, "nodeId": 0, "event": "NODE_ONLINE", "details": "GMC application started." }
{ "t": 2.5, "nodeId": 4, "event": "PERF", "details": "Score=0.85, energy=0.9, linkQ=0.8" }
```

**节点状态（用于监控面板，可从事件或单独 state 文件）：**

```json
{ "t": 10.0, "nodes": { "0": { "energy": 1.0, "linkQ": 1.0, "mobility": 0.0, "score": 0 } } }
```

---

## 三、配色与风格

| 元素 | 颜色 / 说明 |
|------|-------------|
| GMC | 红色 `#C80000` |
| SPN | 黄色/橙 `#E6A800` |
| 普通节点 | 按子网：LTE 蓝 `#2060C0`，Ad-hoc 绿 `#00A050`，战术数据链 橙 `#E07020` |
| 回程链路 | 深红实线 |
| 数据链路 | 蓝色虚线 |
| 背景 | 深灰 `#1E1E1E`，面板 `#2D2D2D`，工业风 |

---

## 四、部署与联动步骤

1. **仅离线**：仿真结束后，在可视化界面选择 `simulation_results/<时间戳>` 或其中导出的 JSON；无需改 NS-3。
2. **导出 JSON**：使用仓库提供的 `export_visualization_data.py`，从 `log/` + `visualization/*.xml` 生成 `topology.json`、`events.json`，供前端加载。
3. **实时（可选）**：启动 `bridge_ws.py`（tail 日志 + WebSocket），再启动 NS-3 仿真；前端选择「实时」并连接 WS。

详细步骤见第四节「部署/调试指南」及仓库内 `visualization/README.md`。
