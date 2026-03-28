# 分层异构 NMS 框架 — 优化方案与 Bug 修复说明

本文档合并**核心优化修改方案**（SPN 选举、TLV、Proxy、FlowMonitor、GMC、NetAnim）与**Bug 修复与功能增强**（GMC 唯一、EPC 区分、loss 文件、SPN_SWITCH、TLV 安全、日志规范）。

---

## 一、已实现项摘要

| 需求/修改 | 状态 | 说明 |
|-----------|------|------|
| SPN 选举 + 回程连线 | ✅ | `ElectInitialSpn()` 按性能评分选举 Adhoc/DataLink SPN；NetAnim 回程 `UpdateLinkDescription` |
| FlowMonitor 丢包率 | ✅ | PERF 日志含 Lost、LossRatio(lost/tx)；flowmon-heterogeneous-loss.txt 每流丢包与公式；按实际传输时长算吞吐/时延；loss 文件含 SubnetType |
| TLV 包格式 | ✅ | `NmsTlv` 命名空间，LTE/Adhoc/DataLink 发送端 TLV 序列化；TLV 长度校验、防缓冲区溢出（Build*Payload 带 bufSize） |
| Proxy 聚合/策略下发 | ✅ | 非 SPN 发往 SPN:5001；SPN 收 5001 聚合+压缩上报 GMC，收 5002 策略转发子网广播；GMC 定期向 SPN:5002 发策略；日志 [PROXY_AGGREGATE]/[PROXY_POLICY]/[POLICY_FORWARD] |
| GMC 单节点 | ✅ | 逻辑与可视化仅 `m_gmcNode.Get(0)`；NetAnim 仅 1 个 GMC；EPC 区分（PGW 标 EPC-PGW、灰色）；节点→SPN 连线 UpdateLinkDescription |
| SPN 动态角色 | ✅ | 失去/重选时打 [SPN_SWITCH]（SendPacket 迟滞切换块） |

---

## 二、SPN 机制与拓扑

- **ElectInitialSpn()**：LTE 固定 eNB；Adhoc/DataLink 对子网内节点用初始评分（能量、拓扑、链路）计算 Score，取最大为 SPN；在 `SetupBackhaul()` 前调用；日志 `[SPN_ELECT] Subnet=X, SPN NodeId=Y, Score=Z`。
- **拓扑与回程**：BuildAdhoc/BuildDataLink 中删除对 m_spnAdhoc、m_spnDatalink 的静态赋值；SetupBackhaul() 以选举出的 SPN 为对端。
- **NetAnim**：GMC–SPN 回程 UpdateLinkDescription "Backhaul:SPN->GMC"；子网内节点–SPN 通过节点描述（如 "LTE-SPN"、"Adhoc-SPN"）与可选 AddCustomLink/UpdateLinkDescription 区分。

---

## 三、FlowMonitor 与 loss 文件

- **传输时长**：有接收时用 timeLastRxPacket - timeFirstTxPacket，否则 timeLastTxPacket - timeFirstTxPacket；durationSec = max(0.001, durationSec)。
- **吞吐/时延/丢包**：thrMbps = (rxBytes*8)/durationSec/1e6；delayMs = delaySum.GetMilliSeconds()/rxPackets（rxPackets>0）；lostPkts = txPackets - rxPackets，lossRatio = lostPkts/txPackets（txPackets>0）。
- **SubnetType**：按源/目的 IP 网段写 Backhaul-LTE/Adhoc/DL、Subnet-Adhoc/DL、LTE、Other。
- **loss 文件表头**：`# FlowID  Src  Dst  SubnetType  TxPkts  RxPkts  LostPkts  LossRatio  Throughput_Mbps  AvgDelay_ms`，并注明公式。

---

## 四、Proxy、TLV 与 GMC

- **Proxy**：普通节点发 TLV 到 SPN:5001；SPN 5001 收包、缓存/去重、聚合压缩后发 GMC:5000；SPN 5002 收策略并转发子网；GMC 周期向各 SPN:5002 发策略，日志 POLICY_FORWARD。
- **TLV**：Type 0x01–0x04（UV-MIB、Delta Energy、标准状态、Policy）；Build*Payload(buf, bufSize, ...) 校验 buf 与 bufSize，返回 0 表示失败则跳过发包。
- **GMC**：仅 m_gmcNode.Get(0)；SetupMonitoring 仅对该节点设 "GMC" 与红色；EPC PGW 设 "EPC-PGW"、灰色；回程与链路描述统一使用 GMC 节点 ID。

---

## 五、SendPacket 中 SPN_SWITCH 与安全

- 当 myScore > m_maxKnownScore + 0.05 时置 m_isProxy = true，若 m_isSpn 则打 `[SPN_SWITCH] Re-elected as SPN (score exceeds threshold).`。
- 当已为 Proxy 且 myScore <= m_maxKnownScore + 0.05 时置 m_isProxy = false，若 m_isSpn 则打 `[SPN_SWITCH] Lost SPN role (score below threshold).`。
- TLV 构建前检查 Build*Payload 返回值，为 0 则跳过发包并 ScheduleNextTx() 返回。

---

## 六、验证方法

1. 编译：`./ns3 build scratch/heterogeneous-nms-framework`  
2. 运行：`./build/scratch/ns3.38-heterogeneous-nms-framework-default --simTime=15`  
3. 日志：nms-framework-log 中应有 [SPN_ELECT]、[SPN_SWITCH]（当 SPN 失去/重选时）、[PROXY_AGGREGATE]、[PROXY_POLICY]、[POLICY_FORWARD]、[PERF]（含 SubnetType、Thr、Delay、Lost、LossRatio）。  
4. loss 文件：flowmon-heterogeneous-loss.txt 表头含 SubnetType，每行含 FlowID、Src、Dst、SubnetType、TxPkts、RxPkts、LostPkts、LossRatio、Throughput_Mbps、AvgDelay_ms。  
5. NetAnim：打开 heterogeneous-nms-framework.xml，仅 1 个节点标 "GMC"，PGW 标 "EPC-PGW"；回程 "Backhaul:*"；子网节点与 SPN 间可有 "DataChannel->SPN" 描述。  
6. XML：`xmllint heterogeneous-nms-framework.xml` 无报错。

---

## 七、关键代码位置（heterogeneous-nms-framework.cc）

- **ElectInitialSpn()**：Run() 中 “ElectInitialSpn” 注释块及框架成员。
- **TLV**：NmsTlv 命名空间及 BuildTlvPacket*、Build*Payload(buf, bufSize)。
- **FlowMonitor / loss**：Run() 末尾 PERF 循环及 flowmon-heterogeneous-loss.txt 写出。
- **SetupMonitoring**：GMC 仅更新一次、EPC-PGW 描述与颜色、GMC–SPN 回程与节点→SPN UpdateLinkDescription、仅业务节点 UpdateNodeSize（见 [06-NetAnim链路与段错误修复.md](06-NetAnim链路与段错误修复.md)）。
- **SendPacket**：SPN_SWITCH 迟滞、TLV 安全校验与 ScheduleNextTx。
