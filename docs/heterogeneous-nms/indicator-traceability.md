# 指标追溯表（论文附录可用）

本文档用于说明“八卦图/性能图表”中每个原始指标的数据来源、计算公式和输出位置，保证可复现与可答辩。

## 1) 原始指标与来源

| 指标 | 原始值来源 | 文件/日志 | 说明 |
|---|---|---|---|
| 吞吐量 `throughputMbps` | FlowMonitor 每流 `rxBytes` 与传输时长 | `flowmon_stats_*.txt`、`FLOW_PERF` | 按业务流统计，单位 Mbps |
| 端到端时延 `delayMs` | FlowMonitor 每流 `delaySum/rxPackets` | `flowmon_stats_*.txt`、`FLOW_PERF` | 单位 ms |
| 丢包率 `lossPct` | `(txPackets-rxPackets)/txPackets` | `flowmon_stats_*.txt`、`FLOW_PERF` | 单位 % |
| 信令开销 `signalingOverheadPct` | 控制流字节占总字节比例 | `KPI_SUMMARY`、`kpi-summary_*.json` | 通过端口分类估计 |
| 自愈时间 `selfHealTimeSec` | 主备切换完成耗时 | `SELF_HEAL_TIME` 日志 | 由 SPN 接管触发 |

## 2) 雷达图归一化规则（0~1）

在 `export_visualization_data.py` 中统一处理：

- 正向指标（越大越好）：
  - `throughputNorm = clamp(throughputMbps / 5.0)`
  - `selfHealSpeedNorm = clamp(1 - selfHealTimeSec / 30.0)`（时间越短速度越高）
- 反向指标（越小越好）：
  - `delayNorm = clamp(1 - delayMs / 200.0)`
  - `lossNorm = clamp(1 - lossPct / 100.0)`
  - `signalingOverheadNorm = clamp(1 - signalingOverheadPct / 100.0)`

其中 `clamp(x)=max(0,min(1,x))`。

## 3) 可视化字段映射

`stats.json` 中：

- `networkRadarMetrics.throughputNorm`
- `networkRadarMetrics.selfHealSpeedNorm`
- `networkRadarMetrics.delayNorm`
- `networkRadarMetrics.lossNorm`
- `networkRadarMetrics.signalingOverheadNorm`
- `networkRadarMetrics.raw.*`（保存原始值，便于审查）

`visualization/app.js` 读取上述字段作为雷达图 5 维指标。

## 4) Baseline 与 HNMP 对比

- baseline：`scenarioMode=compare-baseline`
  - 无抑制/弱聚合/更高上报频率（近似传统洪泛）
- hnmp：`scenarioMode=compare-hnmp`
  - 增强抑制、增大阈值、延长聚合周期、降低控制面频率

建议使用：

```bash
./run_compare.sh
./run_compare_batch.sh
```

分别获得单次对比与多次均值/方差对比结果。

## 5) Wireshark 抓包追溯

- 开启抓包：`--pcap=1 --parsePackets=1`
- 输出目录：`simulation_results/<timestamp>/packet/`
- 文件：
  - `raw_hnmp.pcap`：原始报文（TSN->SPN）
  - `agg_hnmp.pcap`：聚合报文（SPN->GMC）
- 过滤规则：`udp.port == 8080`

