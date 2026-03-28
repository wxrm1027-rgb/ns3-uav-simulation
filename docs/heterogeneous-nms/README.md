# 异构 NMS 仿真平台 — 文档中心

本目录为 **异构无人机集群网络管理仿真平台**（推荐入口 `scratch/heterogeneous-nms/` 下的 `heterogeneous-nms-main`，核心实现位于 `scratch/heterogeneous-nms/framework/heterogeneous-nms-framework.cc`）的**统一说明文档存放位置**。

- **后续所有与该平台相关的说明、设计、使用指南等，请统一生成或链接到本目录。**
- 根目录或其它目录下原有的零散 NMS 相关 `.md` 已在本目录中整理或引用，便于一处查阅。

---

## 文档索引

| 状态 | 文档 | 说明 |
|------|------|------|
| 现行 | [01-框架架构与可视化说明.md](01-框架架构与可视化说明.md) | 框架整体架构、核心模块清单、**自研可视化**（export_visualization_data.py + visualization/）实现说明、数据采集与展示 |
| 现行 | [02-优化与使用说明.md](02-优化与使用说明.md) | 自组网拓扑(Mesh/Star/Tree)、归档目录、pcap 与解析、验证步骤 |
| 现行 | [03-扩展功能说明.md](03-扩展功能说明.md) | 运行命令、双通道、包解析、拓扑配置、plot-flowmon、parse_nms_pcap |
| 现行 | [04-可视化平台设计与部署.md](04-可视化平台设计与部署.md) | 可视化技术选型、数据格式约定、仿真+可视化联动步骤、常见问题 |
| 现行 | [05-时序入网与JSONL片段.md](05-时序入网与JSONL片段.md) | NS-3 时序入网、JSON 配置解析、JSONL 输出、业务路由日志格式 |
| 历史 | [06-NetAnim链路与段错误修复.md](06-NetAnim链路与段错误修复.md) | 历史记录（NetAnim 已下线） |
| 现行 | [07-框架优化方案与Bug修复.md](07-框架优化方案与Bug修复.md) | SPN 选举、TLV、Proxy、FlowMonitor、GMC、SPN_SWITCH、loss 文件与验证 |
| 现行 | [08-节点入网与场景想定验证说明.md](08-节点入网与场景想定验证说明.md) | joinconfig 时序入网、场景想定骨架、可视化时序联动与验证步骤 |
| 现行 | [joinconfig-schema.md](joinconfig-schema.md) | joinconfig 字段说明、合法性校验、运行方式 |
| 现行 | `joinconfig-example.json` | 节点入网配置示例（含扩展字段） |
| 现行 | `scenario-config-example.json` | 场景想定配置示例（scenario_id 等） |

### 当前推荐阅读顺序（现行）

1. `01-框架架构与可视化说明.md`（先了解整体架构）
2. `03-扩展功能说明.md`（常用命令与功能开关）
3. `08-节点入网与场景想定验证说明.md` + `joinconfig-schema.md`（配置与验证）
4. `04-可视化平台设计与部署.md`（前端联动与部署）

---

## 相关路径速查

- **仿真入口（推荐）**：`scratch/heterogeneous-nms/heterogeneous-nms-main.cc` → `HnmsMain()`，构建/运行：`./ns3 build heterogeneous-nms-main`、`./ns3 run heterogeneous-nms-main -- <参数>`
- **唯一维护入口**：仅保留 `heterogeneous-nms-main`；旧单文件入口已删除
- **导出脚本**：`export_visualization_data.py`（项目根目录），输出 `topology.json`、`events.json`、`stats.json`
- **自研可视化前端**：`visualization/index.html`、`app.js`、`styles.css`
- **仿真结果目录**：`simulation_results/<YYYY-MM-DD_HH-MM-SS>/`，下含 `log/`、`visualization/`、`performance/`、`packet/`
