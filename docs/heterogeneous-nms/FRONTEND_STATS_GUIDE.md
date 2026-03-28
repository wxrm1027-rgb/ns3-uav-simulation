# stats.json 前端对接指南

**重要**：业务图表（表格、折线图、雷达图）只有在**前端显式读取** `stats.json` 根节点下的 `businessFeatures`、`flowPerformance`、`networkRadarMetrics` 并绑定到对应组件时才会显示。若未绑定这三个字段，业务图表会为空或不变。

---

## 1. 从 stats.json 提取三个字段

前端加载 `stats.json` 后，从根节点读取：

```javascript
// 假设已通过 fetch/axios 得到 stats 对象
const stats = await fetch('/data/stats.json').then(r => r.json());

// 三个字段均在根节点下
const businessFeatures = stats.businessFeatures || [];   // 业务特征表 + 拓扑高亮
const flowPerformance = stats.flowPerformance || [];    // 折线图时间序列
const networkRadarMetrics = stats.networkRadarMetrics || {};  // 雷达图指标
```

---

## 2. businessFeatures → 表格 + 拓扑高亮

**用途**：业务特征表格、在拓扑图上高亮某条流的 path 连线。

**结构**：对象数组，每项示例：

```json
{
  "flowId": 1,
  "type": "video",
  "priority": 2,
  "src": 1,
  "dst": 22,
  "size": 62500,
  "rateMbps": 2,
  "rateHz": 20,
  "path": [1, 2, 5, 22],
  "label": "video-1"
}
```

**表格绑定（Vue/React 伪代码）**：

```javascript
// 表格列：flowId, type, priority, src, dst, size, rate(Mbps 或 HZ), path
const columns = [
  { key: 'flowId', title: '流ID' },
  { key: 'type', title: '类型' },
  { key: 'priority', title: '优先级' },
  { key: 'src', title: '源节点' },
  { key: 'dst', title: '目的节点' },
  { key: 'size', title: '包大小' },
  { key: 'rate', title: '速率', render: row => row.rateHz != null ? `${row.rateHz}HZ` : `${row.rateMbps}Mbps` },
  { key: 'path', title: '路径', render: row => row.path.join(' → ') }
];
// 数据源
const tableData = businessFeatures;
```

**拓扑高亮 path**：当用户选中某一行（或某条流）时，在拓扑图上将 `path` 中的相邻节点对 `(path[i], path[i+1])` 对应的边高亮（例如加粗、变色）。

```javascript
function getHighlightLinks(selectedFlow) {
  if (!selectedFlow || !selectedFlow.path || selectedFlow.path.length < 2) return [];
  const links = [];
  for (let i = 0; i < selectedFlow.path.length - 1; i++) {
    links.push({ from: selectedFlow.path[i], to: selectedFlow.path[i + 1] });
  }
  return links;
}
```

---

## 3. flowPerformance → 折线图（Echarts）

**用途**：每条流的性能随时间变化：`t`（时间）、`delay`（时延 ms）、`lossRate`（丢包率 0~1）、`throughput`（吞吐 Mbps）。

**结构**：数组，每项为一条流，含 `flowId`、`label`、`path`、`data`（时间序列）：

```json
{
  "flowId": 1,
  "label": "video-1",
  "path": [1, 2, 5, 22],
  "data": [
    { "t": 5.0, "delay": null, "lossRate": null, "throughput": 0.0 },
    { "t": 120.0, "delay": 15, "lossRate": 0.001, "throughput": 2.5 }
  ]
}
```

**Echarts 折线图绑定（示例）**：

```javascript
// 横轴：时间 t
const xData = flowPerformance.flatMap(f => f.data.map(d => d.t)).filter((v, i, a) => a.indexOf(v) === i).sort((a, b) => a - b);

// 每条流一条线：throughput 或 delay
const series = flowPerformance.map((f, idx) => ({
  name: f.label,
  type: 'line',
  data: f.data.map(d => d.t).map((t, i) => {
    const point = f.data[i];
    return point.throughput != null ? point.throughput : '-';
  })
}));

// 或按时间对齐：以 xData 为 xAxis，每个 flow 的 data 按 t 插值/对齐
const option = {
  xAxis: { type: 'value', name: '时间(s)', data: xData },
  yAxis: { type: 'value', name: '吞吐(Mbps)' },
  series
};
```

**多 Y 轴**：若需同时画 delay、throughput、lossRate，可为每条流建多个 series，或使用 Echarts 的 multiple yAxis。

---

## 4. networkRadarMetrics → 雷达图（Echarts）

**用途**：网络整体指标，0–100 归一化，雷达图展示。

**结构**：单个对象，示例：

```json
{
  "networkScale": 32.0,
  "demandMatch": 100.0,
  "responseTime": 92.5,
  "effectiveThroughput": 25.0
}
```

**Echarts 雷达图绑定**：

```javascript
const indicator = [
  { name: '网络规模', max: 100 },
  { name: '需求匹配度', max: 100 },
  { name: '响应时间', max: 100 },
  { name: '有效吞吐', max: 100 }
];
const values = [
  networkRadarMetrics.networkScale,
  networkRadarMetrics.demandMatch,
  networkRadarMetrics.responseTime,
  networkRadarMetrics.effectiveThroughput
];

const option = {
  radar: { indicator },
  series: [{
    type: 'radar',
    data: [{ value: values, name: '网络指标' }]
  }]
};
```

---

## 5. 小结

| 字段 | 用途 | 组件建议 |
|------|------|----------|
| `businessFeatures` | 业务特征表、拓扑 path 高亮 | Table + 拓扑图 highlight |
| `flowPerformance` | 流性能随时间变化 | Echarts line（t / delay / throughput / lossRate） |
| `networkRadarMetrics` | 网络整体 0–100 指标 | Echarts radar |

三个字段均位于 `stats.json` 根节点，直接 `stats.businessFeatures`、`stats.flowPerformance`、`stats.networkRadarMetrics` 即可使用。

---

## 6. 主KPI与调试KPI口径（新增）

为避免控制/探测流干扰业务验收，后端日志与性能文件已拆分为两套口径：

- **主KPI（用于验收）**
  - 日志标签：`PERF_MAIN`、`FLOW_PERF_MAIN`、`KPI_MAIN`
  - 文件：`performance/flowmon_stats_<timestamp>.txt`
  - 统计范围：仅 `scenario_config.json` 中 `business_flows` 对应业务流

- **调试KPI（用于排障）**
  - 日志标签：`PERF_DEBUG`、`FLOW_PERF_DEBUG`、`KPI_DEBUG`
  - 文件：`performance/flowmon_debug_<timestamp>.txt`
  - 统计范围：非业务流（控制/探测/回程等）

前端若用于“是否达标”展示，应优先读取主KPI口径；调试KPI建议放在“高级诊断/开发模式”面板中显示。

---

## 7. 状态驱动KPI字段（新增）

后端 `export_visualization_data.py` 现会自动把
`performance/state-driven-kpi-summary_<timestamp>.json`
写入 `stats.json` 根节点字段：

```javascript
const stateDrivenKpi = stats.stateDrivenKpi || {};
```

推荐展示指标：

- `controlOverheadBps`：控制开销（B/s）
- `suppressionRatePct`：抑制率（%）
- `avgReportIntervalSec`：平均上报间隔（s）
- `scheduleDecisions` / `scheduleTriggered` / `scheduleSuppressed`
- `aggregateSuppressed` / `aggregateRawBytes` / `aggregateSentBytes`

示例：

```json
{
  "controlOverheadBps": 79.5,
  "suppressionRatePct": 74.6988,
  "avgReportIntervalSec": 5.0,
  "scheduleDecisions": 83,
  "scheduleTriggered": 21,
  "scheduleSuppressed": 62,
  "aggregateSuppressed": 1,
  "aggregateRawBytes": 294,
  "aggregateSentBytes": 303
}
```
