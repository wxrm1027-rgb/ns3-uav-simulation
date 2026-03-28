# NetAnim 连线样式扩展与段错误修复

> 历史归档说明：当前主线仿真已下线 NetAnim，本文件仅保留为历史问题与修复记录，不作为当前运行流程指南。

本文档合并**NetAnim 链路样式扩展**（XML 规范与 ns-3 侧实现）与**设置节点图标大小段错误修复**说明。

---

## 第一部分：NetAnim 连线样式扩展

### 一、当前 XML 链路规范（未扩展前）

- **元素**：`<link>`（初始拓扑）、`<linkupdate>`（运行时更新）。
- **已有属性**：`fromId`, `toId`；`fd`, `td`；`ld`（链路描述，查看器仅当文本显示，不参与绘制样式）。要让连线按颜色/线宽/实线虚线显示，需扩展 **XML 规范（ns-3 写出）** 和 **NetAnim 查看器（解析并绘制）**。

### 二、ns-3 侧扩展（本仓库已完成）

- **animation-interface.h**：在 `LinkProperties`、`CustomLinkElement` 中增加可选样式字段：`linkColorR/G/B`、`linkWidth`、`linkDashed`；增加带样式的 `AddCustomLink(fromId, toId, description, colorR, colorG, colorB, widthPx, dashed)`。
- **animation-interface.cc**：在 `WriteLinkProperties` 里把自定义链路的样式写入 `LinkProperties`；在 `WriteXmlLink` 里把样式写成 XML 属性 `lr`、`lg`、`lb`、`lw`、`dashed`。
- **heterogeneous-nms-framework.cc**：回程用 `UpdateLinkDescription`/`UpdateLinkStyle`，数据链路用 `AddCustomLink(..., 0, 0, 255, 1.0, true)` 等。

生成的 XML 示例：`<link fromId="0" toId="4" ld="SPN-GMC-Backhaul" lr="255" lg="0" lb="0" lw="3" />`；`<link ... ld="Node-SPN-Data" lr="0" lg="0" lb="255" lw="1" dashed="1" />`。旧版 NetAnim 不识别这些属性也不会报错；若使用本仓库提供的 `view_netanim_links.py` 生成 `topology.html` 在浏览器中查看，即可看到红色实线与蓝色虚线。

### 三、NetAnim 查看器侧（可选）

若需在 NetAnim 可执行程序中看到样式：解析 `<link>` 的 `lr`、`lg`、`lb`、`lw`、`dashed`；存储到内部链路结构；绘制时用 `QPen` 设置颜色、线宽、DashLine/SolidLine。详见原 `docs/NETANIM-LINK-STYLE-EXTENSION.md`（已整理到本目录前身）。

---

## 第二部分：设置节点图标大小段错误修复

### 一、段错误根因（按优先级）

1. **【主因】XML 中缺失部分 nodeId 的 `<node>` 导致 NetAnim 按 id 索引越界**  
   EPC 节点 1/2/3 被配置为“隐藏”后，AnimationInterface 不再写入 `<node id="1">` 等，仅写入业务节点 0, 4–18。NetAnim 若按 nodeId 作数组下标，则下标 1、2、3 未初始化，执行「设置节点图标大小」时访问这些槽位触发段错误。

2. **【次因】隐藏节点未写入 size**  
   若仅对非隐藏节点调用 WriteNodeSizes，则 XML 中不存在 id=1/2/3 的 `<nu p="s">`，统一设置图标大小时可能访问缺失 id 导致越界。

3. **【可能】坐标 NaN/Inf、脚本对不存在的节点调用 UpdateNodeSize、NetAnim 与 ns-3 版本不匹配**，会放大上述问题。

### 二、核心代码修改

**AnimationInterface（src/netanim/model/animation-interface.cc）**

- `#include <cmath>`，用于 `std::isnan` / `std::isinf`。
- **WriteNodes()**：对隐藏节点不再跳过，改为写入**占位节点** `WriteXmlNode(nodeId, n->GetSystemId(), 0, 0)`，保证 XML 中存在 id=1,2,3 的 `<node>`；非隐藏节点位置若 NaN/Inf 则置 0。
- **WriteNodeSizes()**：对隐藏节点写入 **size 0**，保证每个 nodeId 在 XML 中都有对应 size 记录；非隐藏写 1,1。
- **WriteXmlNode / WriteXmlUpdateNodePosition / WriteXmlUpdateNodeSize**：对 locX/locY、x/y、width/height 做 NaN/Inf 及负数检查，非法则置 0 或 1。
- **MobilityCourseChangeTrace / MobilityAutoCheck**：在写位置前对 v.x、v.y 做 NaN/Inf 检查。

**SetupMonitoring()（scratch/heterogeneous-nms/framework/heterogeneous-nms-framework.cc）**

- 使用 `NodeList::GetNNodes()` 校验，若为 0 则 return。
- 仅对 EPC 节点 1/2/3 调用 `AddHiddenNode(1/2/3)`，**不对 EPC 调用** `UpdateNodeDescription` / `UpdateNodeColor` / `UpdateNodeSize`。
- 仅对业务节点（GMC、LTE eNB/UE、Adhoc、DataLink）且非空时调用 `m_anim->UpdateNodeSize(node, 1.0, 1.0)`；不对 nodeId 1、2、3 做任何 Update* 调用。

### 三、验证

- 编译：`./ns3 build netanim scratch/heterogeneous-nms-framework`；运行生成 XML。
- 检查 XML：存在 `<node id="0">` … `<node id="18">`（含 1,2,3 的占位）；每个 id 均有 `<nu p="s" ...>`；locX/locY 无 NaN/Inf。
- NetAnim 打开该 XML，执行「设置节点图标大小」：无段错误；业务节点 0、4–18 可见，EPC 占位 1、2、3 在视野外且 size 为 0。

### 四、兼容性

- AnimationInterface 生成的 XML 符合 netanim **3.108** 格式；建议使用与 ns-3 同源或兼容 netanim 3.108 的 NetAnim 可执行文件。
- 修改在 **ns-3.38** 下测试通过；若使用其他 ns-3 版本，需确认 AnimationInterface 中 AddHiddenNode/AddCustomLink/DoInitialWrite 及 WriteNodes/WriteNodeSizes 的签名与调用方式是否一致。

---

## 修改位置速查

| 文件 | 内容 |
|------|------|
| `src/netanim/model/animation-interface.h` | LinkProperties、CustomLinkElement 样式字段；AddCustomLink 重载 |
| `src/netanim/model/animation-interface.cc` | #include \<cmath\>；WriteNodes 占位+NaN 处理；WriteNodeSizes 隐藏节点 size 0；WriteXml* 合法性检查；Mobility* 坐标检查；WriteXmlLink 写出 lr/lg/lb/lw/dashed |
| `scratch/heterogeneous-nms/framework/heterogeneous-nms-framework.cc` | SetupMonitoring：NodeList::GetNNodes() 校验；仅业务节点 UpdateNodeSize；EPC 仅 AddHiddenNode；AddCustomLink/UpdateLinkStyle 区分回程与数据链路 |
