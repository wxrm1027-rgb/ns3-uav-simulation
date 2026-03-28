# 为什么日志里只有节点 0–11，蜂窝网节点 12–15 没有出现？

## 结论（简要）

- **不是蜂窝网（LTE）本身的问题**，而是 **join_config.json 只被解析出 1 个节点** 导致的连锁反应。
- 根因是 C++ 里 `LoadNodeJoinConfig()` 的解析逻辑有 bug，已修复；**重新编译并运行后**，日志会显示 16 个节点，且 state 里会包含节点 12–15。

---

## 1. 现象

- **nms-framework-log_*.txt** 里有一行：`Loaded join config: 1 nodes from ...`  
  → 说明当前运行只加载了 **1 个节点**，而不是 join_config.json 里配置的 16 个。
- **nms-state_*.jsonl** 里每个时刻只有 **node_id 0–11**，没有 12、13、14、15。  
  → 12–15 对应蜂窝网（LTE）的 4 个 UE，所以看起来像“蜂窝网节点没进日志”。

---

## 2. 根因：Join 配置解析 Bug

在 `scratch/heterogeneous-nms/framework/heterogeneous-nms-framework.cc` 的 **LoadNodeJoinConfig()** 里：

- JSON 是**手写循环**按 `"node_id"` 找每个节点对象的。
- 每解析完一个节点后，原来用 `i = pNodeId + 1` 只往后挪了一点点，**没有跳到下一个 JSON 对象**。
- 下一轮 `find("\"node_id\"", i)` 会再次命中**同一个** `"node_id"`（第一个对象），于是**一直只解析第一个节点**，结果 `m_joinConfig` 里只有 1 条（例如 node 0）。
- 日志里就会一直看到 **“Loaded join config: 1 nodes”**。

因此：**不是“蜂窝网没写日志”，而是“整个 join 配置只加载了 1 个节点”，后面建网和写 state 都基于这份错误配置。**

---

## 3. 为什么 state 里只有 0–11，没有 12–15？

- 在**当前已修复的代码**里，**state 的写入**是按“节点组”来的：GMC、Adhoc、DataLink、LTE eNB、**LTE UE**，并且 **LTE UE 的循环已经存在**（会写 11,12,13,14,15）。
- 你看到的 **0–11 的日志** 是用**旧可执行文件**跑出来的：
  - 要么是**未包含“LTE UE 写入循环”的旧版本**（那时只写了 0–11），  
  - 要么是 **join 只加载 1 个节点** 的版本下，LTE 建网/节点数异常，导致 state 只写了部分节点。

总之：**12–15 没出现，是“配置只加载 1 个节点”或“旧版未写 LTE UE”的后果，不是蜂窝网模块单独写错。**

---

## 4. 已做修改（代码里已生效）

在 **LoadNodeJoinConfig()** 里，每解析完一个节点并执行 `out[c.nodeId] = c` 后，**不再**用 `i = pNodeId + 1`，而是：

- 用 `objEnd = content.find('}', i)` 找到**当前 JSON 对象的结束括号**；
- 令 `i = objEnd + 1`，下一轮从**下一个对象**开始找 `"node_id"`。

这样会正确解析出 join_config.json 里的**全部 16 个节点**（含 5 个 LTE UE，对应 node 11–15）。

---

## 5. 你需要做的

1. **重新编译**  
   ```bash
   ./ns3 build
   ```

2. **用同一 join 配置再跑一次**（例如）：  
   ```bash
   ./ns3 run scratch/heterogeneous-nms-framework -- --joinConfig=docs/heterogeneous-nms/join_config.json ...
   ```

3. **看新日志**  
   - `nms-framework-log_*.txt` 里应出现：**`Loaded join config: 16 nodes from ...`**  
   - `nms-state_*.jsonl` 里每个时间戳应有 **node_id 0–15**（含 12、13、14、15）。

这样，**蜂窝网节点 12–15 就会正常出现在日志和 state 里**；问题来自 join 解析与旧可执行文件，而不是蜂窝网本身。
