# 异构 NMS 仿真可视化 — 使用与部署说明

## 功能概览

- **拓扑展示**：节点（GMC/SPN/普通）与链路，按子网筛选；**时序入网**（未入网灰色，入网后 GMC 红、SPN 橙、LTE 蓝、Ad-hoc 绿、数据链黄）；**链路区分**（回程红粗实线、数据蓝细虚线）；支持节点时序位置；图例标注。
- **事件回放**：底部时间轴、播放/暂停/重置，事件列表点击定位；仅两端入网后显示连线。
- **统计图表**：趋势（能量/链路质量/Score 等）、性能（吞吐/时延/丢包）、SPN 选举次数；可导出图表 PNG。
- **导出**：事件 CSV、当前图表 PNG。

## 与 NS-3 联动（离线）

### 1. 运行 NS-3 仿真

仿真结束后会在 `simulation_results/<时间戳>/` 下生成：

- `log/nms-framework-log_<时间戳>.txt` — NmsLog 事件日志
- `visualization/heterogeneous-*.xml` — 拓扑/节点位置（若已启用 NetAnim 或自定义导出）

### 2. 导出可视化数据

在项目根目录执行：

```bash
python3 export_visualization_data.py simulation_results/<时间戳> -o visualization/data
```

将生成：

- `visualization/data/topology.json` — 节点（含 joinTime、可选 positions 时序）、去重链路（含 t）
- `visualization/data/events.json` — 带时间戳的事件列表
- `visualization/data/stats.json` — FlowMonitor 流统计、时序、SPN 选举次数
- `visualization/data/meta.json` — 元信息（可选）

### 3. 打开可视化界面

**方式 A：本地 HTTP 服务（推荐）**

```bash
cd visualization
python3 -m http.server 8000
```

浏览器访问：`http://localhost:8000/`，打开 `index.html`。

- **加载数据**：点击「加载」，在文件选择框中**多选** `data/topology.json` 和 `data/events.json`，确定后即可看到拓扑与事件。
- **路径加载**：若将 `data` 放在服务根目录下，可在输入框填写 `data`，点击「加载」，通过相对路径请求 `data/topology.json` 与 `data/events.json`（仅在同源 HTTP 下有效）。

**方式 B：直接打开 index.html（file://）**

- 只能通过「加载」**选择文件**：在文件选择对话框中同时选中 `topology.json` 与 `events.json`（需先通过步骤 2 导出到同一目录，再在对话框中多选这两个文件）。
- 路径输入框在 file 协议下无法访问本地路径，可忽略。

### 4. 操作说明

- **子网筛选**：勾选/取消 GMC、LTE、Ad-hoc、战术数据链，拓扑仅显示对应节点与链路。
- **回放**：拖动时间轴滑块或点击「播放」推进仿真时间；事件列表中当前时刻对应事件高亮，点击某条事件可跳转到该时刻。
- **导出事件 CSV**：点击「导出事件 CSV」下载 `nms_events.csv`。

## 可选：实时模式（WebSocket 桥接）

若需在仿真运行过程中实时看到事件与拓扑更新：

1. **NS-3 侧**：在 `NmsLog` 或关键事件处增加 UDP/TCP 发送，将结构化行（如 JSON）发往本机某端口；或保持写日志文件，由桥接进程 tail 该文件。
2. **桥接脚本**：使用 Python/Node 等监听端口或 `tail -f` 日志 → 解析为 `{ t, nodeId, event, details }` → 通过 WebSocket 推送到浏览器。
3. **前端**：在 `app.js` 中增加 WebSocket 客户端，收到消息后 `state.events.push(...)` 并刷新时间轴与事件列表（当前版本未实现，可按上述思路扩展）。

## 目录结构

```
visualization/
├── index.html    # 主页面
├── styles.css    # 样式（工业风配色）
├── app.js        # 拓扑、回放、筛选、图表、导出逻辑
└── README.md     # 本说明
```

数据导出后建议放在 `visualization/data/`，便于与「路径加载」配合使用。
