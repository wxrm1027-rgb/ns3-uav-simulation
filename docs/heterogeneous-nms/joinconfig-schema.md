# joinconfig 配置文件说明（节点时序入网）

## 格式

- 优先使用 **JSON** 格式（数组，每项一个节点）。
- 支持字段：见下表；未写字段使用默认或由仿真自动分配。

## 字段说明

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `node_id` | 整数 | 是 | 节点唯一 ID，需与 NS-3 创建顺序得到的 NodeId 一致（GMC=0, Adhoc=1–5, DataLink=6–9, LTE eNB/UE=13–18 等） |
| `join_time` | 小数 | 否* | 入网时间（秒），支持小数即毫秒精度；与 `join_time_ms` 二选一 |
| `join_time_ms` | 整数 | 否* | 入网时间（毫秒），若存在则覆盖 `join_time` |
| `init_pos` | [x,y,z] | 否 | 初始三维坐标（米），适配空域 |
| `speed` | 小数 | 否 | 移动速度（m/s），0 表示静止；仅 ConstantVelocity 模型生效 |
| `type` | 字符串 | 否 | 如 GMC/SPN/TSN/普通，用于日志与可视化 |
| `subnet` | 字符串 | 否 | 如 LTE/Adhoc/DataLink，用于日志与可视化 |
| `ip` | 字符串 | 否 | 静态 IP；空表示动态分配（当前实现仍由各子网地址池分配） |
| `initial_rate_mbps` | 小数 | 否 | 入网初始传输速率（Mbps），≤0 表示不覆盖（预留） |
| `initial_energy` | 小数 | 否 | 初始能量 [0,1]，<0 表示不覆盖；用于 UV-MIB 与能耗逻辑 |
| `initial_energy_mah` | 小数 | 否 | 初始能量 mAh（可选，用于显示或后续能耗模型） |
| `initial_link_quality` | 小数 | 否 | 初始链路质量 [0,1] 或 RSSI 归一化，<0 表示不覆盖 |

\* `join_time` 与 `join_time_ms` 至少填一个；若填 `join_time_ms` 则入网时间 = join_time_ms/1000 秒。

## 合法性校验

- **node_id 唯一**：map 按 node_id 存储，重复项后者覆盖前者。
- **IP 冲突**：若配置了 `ip`，则同一文件中不得出现重复 IP。
- **时间戳有序**：按 `join_time` 排序后须非严格递增（允许相等）。
- **数值范围**：`initial_energy`、`initial_link_quality` 若配置则须在 [0,1]。

校验失败时在日志中输出 `JOINCONFIG_VALIDATION` 警告，不中断仿真。

## 运行方式

```bash
./ns3 run heterogeneous-nms-main -- --joinConfig=path/to/joinconfig.json
```

示例见 `joinconfig-example.json`。
