# NS-3 时序入网与 JSONL 日志输出 — 代码片段

本文档提供在 NS-3 仿真中实现「按 JSON 配置时序入网」和「JSONL 格式状态日志」的 C++ 代码片段，供集成到现有异构 NMS 框架使用。

## 一、JSON 配置文件格式

节点入网配置示例（如 `node_join_config.json`）：

```json
[
  {"node_id": 0, "type": "GMC", "subnet": "LTE", "join_time": 0.0, "init_pos": [0, 0, 50], "speed": 0},
  {"node_id": 1, "type": "SPN", "subnet": "TSN", "join_time": 1.2, "init_pos": [100, 200, 50], "speed": 5},
  {"node_id": 2, "type": "SPN", "subnet": "Adhoc", "join_time": 0.5, "init_pos": [150, 250, 50], "speed": 3}
]
```

字段说明：`node_id` 节点 ID；`type` 为 GMC/SPN/TSN/普通；`subnet` 为 LTE/Adhoc/TSN/DataLink；`join_time` 入网时间（秒）；`init_pos` 为 [x,y,z]（米）；`speed` 移动速度（m/s，可选）。

## 二、JSON 配置解析（C++）

需包含 rapidjson 或使用 ns-3 可用的简单解析方式。以下为基于标准 fstream + 手写解析的轻量实现（不依赖第三方库）：

（详见原文档或 `heterogeneous-nms-framework.cc` 中 `NodeJoinConfig`、`LoadNodeJoinConfig()` 实现。）

## 三、按 join_time 时序启动节点

在仿真主程序中，根据当前仿真时间与 `join_time` 决定是否启动该节点应用/入网状态：

- 在 `Simulator::Schedule(Seconds(joinTime), &StartNodeApplication, node, config)` 中在 `joinTime` 时刻启动该节点的应用并设置初始位置，同时将节点状态置为「已入网」并输出日志。

## 四、JSONL 状态日志输出

要求每行一个 JSON 对象，字段包括：`timestamp`、`node_id`、`type`、`subnet`、`join_state`、`pos_x`/`pos_y`/`pos_z`、`ip`、`energy`、`link_quality`。在 NmsLog 或全局探针中周期性（或事件驱动）写文件。框架中已实现 `WriteJsonlStateLine()`、`WriteAllNodesJsonl()`，写入 `simulation_results/<时间戳>/log/nms-state_*.jsonl`。可视化导出脚本会解析该 JSONL 生成 `nodeStatesByTime` 供前端展示。

## 五、与现有 NmsLog 的衔接

- 保留现有 `NmsLog` 文本格式（`[ts] [Node id] [EVENT_TYPE] details`），用于事件回放与拓扑推断。
- 若启用时序入网，在节点到达 `join_time` 时除执行应用启动/位置设置外，可同时写一条 NmsLog：`[join_time s] [Node node_id] [NODE_ONLINE] type=..., subnet=..., join_time=...`，便于导出脚本解析 `joinTime` 与角色。
- JSONL 与 NmsLog 可并行输出：NmsLog 供 events.json，JSONL 供 nodeStatesByTime 与节点详情（IP、能量、链路质量等）。

## 六、TSN/SPN 类型与子网

- 在配置与日志中 `type` 可取 GMC、SPN、TSN、普通；`subnet` 可取 LTE、Adhoc、TSN、DataLink。
- 可视化端根据 `subnet` 与 `type` 区分 TSN（紫色）与 SPN（橙色），并支持 SPN/TSN 筛选与拓扑标注。

## 七、自组网网状拓扑（Adhoc/TSN）

- Adhoc/TSN 子网内应保证节点两两可达（Mesh），无孤立节点。导出脚本会根据子网内节点自动补全两两链路（`ensure_mesh_links`），供前端绘制；链路可带 `link_quality`，前端线宽与质量正相关。

## 八、业务路由可视化日志

业务触发时输出路由路径，供可视化高亮显示。在发包/路由决策处写 NmsLog：

```cpp
// 业务开始：记录 flowId、类型、路径（源→中间→目标）
NMS_LOG_INFO(0, "ROUTE_START", "flowId=1 type=video path=1,2,5");

// 业务结束
NMS_LOG_INFO(0, "ROUTE_END", "flowId=1");
```

- 格式约定：`ROUTE_START` / `FLOW_START` 的 details 中可含 `flowId=数字`、`type=video|data|cmd`、`path=节点1,节点2,节点3`（逗号分隔）；`ROUTE_END` / `FLOW_END` 含 `flowId=数字`。
- 导出脚本会解析为 `stats.json` 的 `businessFlows`，前端在拓扑上按时间范围绘制高亮路径，并可勾选显示/隐藏。
