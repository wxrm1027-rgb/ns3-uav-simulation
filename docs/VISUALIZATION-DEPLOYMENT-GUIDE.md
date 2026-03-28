# 异构 NMS 仿真可视化 — 部署运行指南

## 一、依赖与环境

### 1.1 可视化前端（Web）

- **浏览器**：Chrome / Firefox / Edge（支持 ES5+、ECharts）
- **本地 HTTP 服务**（用于加载 data 路径）：Python 3 自带 `http.server`，无需额外安装

```bash
# 无需安装，直接使用
python3 -m http.server 8000
```

### 1.2 导出脚本（Python）

- **Python**：3.6+
- **标准库**：`json`、`os`、`re`、`xml.etree.ElementTree`、`argparse`，无需 pip 安装

### 1.3 NS-3 仿真

- **NS-3**：3.38 或与当前仓库一致版本
- **编译**（项目根目录）：
  ```bash
  ./ns3 configure --enable-examples
  ./ns3 build scratch/heterogeneous-nms-framework
  ```
- **运行**：
  ```bash
  ./ns3 run scratch/heterogeneous-nms-framework
  ```
- **可选**：若使用 JSON 入网配置，需在 NS-3 中集成 JSON 解析（如 RapidJSON）并实现时序启动与 JSONL 输出（参见 `docs/NS3-TIMED-JOIN-SNIPPETS.md`）。业务路由高亮需在业务触发时输出 `ROUTE_START` / `ROUTE_END` 事件（见片段文档）。

---

## 二、JSON 配置文件示例（NS-3 时序入网）

NS-3 侧若支持从 JSON 读入节点入网时间与初始位置，可参考以下格式（存为 `node_join_config.json`）：

```json
[
  {"node_id": 0, "type": "GMC", "subnet": "LTE", "join_time": 0.0, "init_pos": [0, 0, 50], "speed": 0},
  {"node_id": 1, "type": "SPN", "subnet": "TSN", "join_time": 1.2, "init_pos": [100, 200, 50], "speed": 5},
  {"node_id": 2, "type": "SPN", "subnet": "Adhoc", "join_time": 0.5, "init_pos": [150, 250, 50], "speed": 3}
]
```

- `node_id`：节点 ID  
- `type`：GMC / SPN / TSN / 普通  
- `subnet`：LTE / Adhoc / TSN / DataLink  
- `join_time`：入网时间（秒）  
- `init_pos`：[x, y, z] 初始位置（米）  
- `speed`：移动速度（m/s）

---

## 三、仿真 + 可视化联动运行步骤

### 3.1 离线模式（推荐）

1. **运行 NS-3 仿真**  
   执行现有异构 NMS 仿真，得到目录：  
   `simulation_results/<时间戳>/`  
   内含 `log/`、`performance/`、`visualization/`（若启用 NetAnim 等）。

2. **导出可视化数据**  
   在项目根目录执行：

   ```bash
   python3 export_visualization_data.py simulation_results/<时间戳> -o visualization/data
   ```

   将生成：  
   - `visualization/data/topology.json`（节点、链路、joinTime、可选 positions）  
   - `visualization/data/events.json`（事件列表）  
   - `visualization/data/stats.json`（FlowMonitor、时序、SPN 选举、可选 nodeStatesByTime）  
   若仿真输出 JSONL 状态日志（见 NS-3 片段文档），导出脚本会解析并写入 `nodeStatesByTime` 与节点 ip/energy/link_quality。

3. **启动本地 HTTP 服务并打开界面**  

   ```bash
   cd visualization
   python3 -m http.server 8000
   ```

   浏览器访问：`http://localhost:8000/`  
   - **加载**：多选 `data/topology.json`、`data/events.json`、`data/stats.json`；或路径输入 `data` 后点击「加载」。

4. **操作**  
   - **子网/类型筛选**：勾选 GMC、LTE、Ad-hoc、战术数据链、SPN、TSN；拓扑**仅显示**选中类型的节点与链路，取消某类即隐藏该类。  
   - **时间轴**：底部拖拽或播放/暂停/重置，精准定位到任意仿真时刻，拓扑与图表同步更新。  
   - **节点详情**：点击拓扑中的节点，弹出**悬浮信息面板**（深色主题），显示 Node ID、IP、节点类型、子网、入网状态、剩余能量、链路质量、移动速度、Score、当前承载业务类型、发包/收包；**点击面板外空白处关闭**。  
   - **业务流筛选**：若 stats 含 businessFlows，左侧可勾选要显示的业务流，拓扑上以高亮路径显示；业务仅在 startTime～endTime 内显示。  
   - **统计图表**：切换趋势/性能/SPN 选举，坐标轴 ≥12px、网格线、滚轮缩放与拖拽，可导出 PNG/CSV。

### 3.2 实时模式（可选）

- NS-3 将状态以 JSONL 或 Socket 推送至本机某端口；  
- 本地运行桥接脚本（如 Python）读取端口或 tail JSONL，再通过 WebSocket 推给浏览器；  
- 前端连接 WebSocket 后追加事件/状态并刷新拓扑与图表。  
当前仓库默认提供离线流程；实时模式需自行实现桥接与前端 WebSocket 逻辑。

---

## 四、界面效果说明

- **拓扑区（约 38% 高度）**  
  - 节点按类型着色：GMC 红、SPN 橙、TSN 紫、LTE 蓝、Ad-hoc 绿、战术数据链黄、未入网灰。  
  - SPN/TSN 节点下方显示「SPN」「TSN」标签。  
  - 回程链路：红色粗实线；数据链路：蓝色细虚线；链路粗细可与链路质量相关。  
  - 未到入网时间的节点为灰色；到达 joinTime 后显示为对应类型颜色；仅两端入网后才绘制连线。  
  - 图例在拓扑上方，标注节点类型与链路类型。

- **下方内容区（约 62%）**  
  - 左侧：子网/类型筛选、事件列表、**业务流筛选**（勾选要显示的业务流）、节点详情占位。  
  - 点击节点后弹出**悬浮节点详情面板**（深色），含 Node ID、IP、节点类型、子网、入网状态、剩余能量、链路质量、移动速度、Score、当前承载业务类型、发包/收包；点击遮罩关闭。  
  - 右侧：统计图表（趋势/性能/SPN 选举），坐标轴字体 ≥12px，网格线，dataZoom，导出 PNG/CSV。

- **底部**  
  - 仿真时间轴、当前时间、播放/暂停/重置。

---

## 五、常见问题排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| 拓扑为空 | 未加载 topology.json 或 nodes 为空 | 先运行导出脚本，再加载 data 目录下三个 JSON；确认仿真结果目录含 log 与（可选）visualization XML。 |
| 无节点详情 | topology 中无 ip/energy 等，且无 nodeStatesByTime | 若使用 JSONL，确保 NS-3 输出 JSONL 且导出脚本能读到该文件；检查 stats.json 是否含 nodeStatesByTime。 |
| 路径加载失败 | 直接打开 file:// 或路径错误 | 必须通过「加载」选文件，或使用 `python3 -m http.server` 后在同源下用路径 `data` 加载。 |
| 图表不显示 | ECharts 未加载或容器高度为 0 | 确认网络可访问 ECharts CDN；检查 `.chart-box.active` 容器有高度。 |
| Adhoc/TSN 无网状连线 | 导出脚本未补 mesh | 导出脚本会对 Adhoc/TSN 子网补全两两链路；确认 export_visualization_data.py 已更新并重新导出。 |
| SPN/TSN 筛选无效 | 旧版 nodeVisible 逻辑 | 已改为「仅显示勾选类型」：节点至少属于某一勾选类型才显示；确认已加载最新 app.js。 |
| 业务流不显示 | 无 ROUTE_START/ROUTE_END 事件 | NS-3 需在业务触发时输出 ROUTE_START details 含 flowId、type、path；见 docs/NS3-TIMED-JOIN-SNIPPETS.md。 |

---

## 六、文件清单

- `visualization/index.html` — 主页面（拓扑 ≤40%，图表 ≥60%，SPN/TSN 筛选与节点详情面板）  
- `visualization/styles.css` — 样式（TSN 紫色、布局、图例）  
- `visualization/app.js` — 拓扑绘制、时序入网、节点点击详情、图表 dataZoom/导出、nodeStatesByTime 支持  
- `export_visualization_data.py` — 导出 topology/events/stats，支持 JSONL、mesh 链路、nodeStatesByTime  
- `docs/NS3-TIMED-JOIN-SNIPPETS.md` — NS-3 时序入网与 JSONL 输出代码片段  
- `docs/VISUALIZATION-DEPLOYMENT-GUIDE.md` — 本部署运行指南  
