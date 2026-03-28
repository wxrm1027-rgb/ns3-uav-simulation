# 如何扩展 NetAnim 查看器与 XML 规范（连线样式）

> 历史归档说明：当前主线仿真已下线 NetAnim，本文件仅保留为历史实现记录与参考，不作为当前运行流程指南。

**本仓库已完成 ns-3 侧扩展**：XML 中 `<link>` 已支持可选属性 `lr`、`lg`、`lb`、`lw`、`dashed`，仿真中通过 `SetLinkStyle()` 与 `AddCustomLink(..., colorR, colorG, colorB, widthPx, dashed)` 写入。**无需改 NetAnim 即可在浏览器中看效果**：运行  
`python3 view_netanim_links.py simulation_results/<时间戳>/visualization/heterogeneous-nms-framework_<时间戳>.xml -o topology.html`  
后在浏览器打开 `topology.html`，即可看到红色实线（SPN–GMC）与蓝色虚线（Node–SPN）。

NetAnim 分为两部分：

1. **ns-3 侧**：`src/netanim/model/animation-interface.{h,cc}` 中的 `AnimationInterface`，在仿真时生成 **XML 跟踪文件**（含节点、链路、包事件等）。
2. **查看器侧**：独立的 **NetAnim 可执行程序**（Qt 项目），**离线**读取该 XML 并绘制拓扑与动画。

要让连线在 NetAnim 中按“颜色 / 线宽 / 实线虚线”显示，需要同时扩展 **XML 规范（ns-3 写出）** 和 **查看器（解析并绘制）**。

---

## 一、当前 XML 链路规范（未扩展前）

- **元素**：`<link>`（初始拓扑）、`<linkupdate>`（运行时更新描述）。
- **已有属性**（见 `AnimationInterface::WriteXmlLink`）：
  - `fromId`, `toId`：端点节点 ID
  - `fd`, `td`：端点描述（from/to description）
  - `ld`：链路描述（link description），当前唯一与“样式”相关的字段，且查看器只当**文本**显示，不参与绘制样式。

ns-3 中对应结构：

- `LinkProperties`：仅含 `fromNodeDescription`, `toNodeDescription`, `linkDescription`。
- `CustomLinkElement`：仅含 `fromId`, `toId`, `description`。

因此**仅改描述字符串**无法让查看器画出不同颜色/线宽/线型，必须增加**结构化属性**并在查看器里用它们驱动 `QPen` 等绘制逻辑。

---

## 二、扩展方案概览

| 步骤 | 位置 | 内容 |
|------|------|------|
| 1 | ns-3：`animation-interface.h` | 在 `LinkProperties`、`CustomLinkElement` 中增加可选“样式”字段；增加带样式的 `AddCustomLink` 重载或 `UpdateLinkStyle`。 |
| 2 | ns-3：`animation-interface.cc` | 在 `WriteLinkProperties` 里把自定义链路的样式写入 `LinkProperties`；在 `WriteXmlLink` 里把样式写成 XML 属性（如 `lr`/`lg`/`lb`/`lw`/`dashed`）。 |
| 3 | NetAnim 查看器源码 | 解析 `<link>` 的上述新属性；在绘制链路的代码里根据属性设置 `QPen`（颜色、线宽、实线/虚线）。 |

以下按“ns-3 扩展”和“查看器扩展”两部分具体说明。

---

## 三、ns-3 侧：扩展 XML 规范与写入

### 3.1 扩展 `LinkProperties` 与 `CustomLinkElement`（animation-interface.h）

在 `LinkProperties` 中增加可选样式（用“未设置”表示使用默认）：

```cpp
/// LinkProperties structure
struct LinkProperties
{
    std::string fromNodeDescription;
    std::string toNodeDescription;
    std::string linkDescription;
    // 可选连线样式（-1 或 0 表示未设置，使用查看器默认）
    int linkColorR = -1;
    int linkColorG = -1;
    int linkColorB = -1;
    double linkWidth = 0;   // 0 表示默认
    bool linkDashed = false;
};
```

在 `CustomLinkElement` 中同样增加样式字段，并在**公开 API** 中增加带样式的接口，例如：

```cpp
struct CustomLinkElement
{
    uint32_t fromId;
    uint32_t toId;
    std::string description;
    int colorR = -1, colorG = -1, colorB = -1;
    double widthPx = 0;
    bool dashed = false;
};

// 在 class AnimationInterface 的 public 区增加：
void AddCustomLink(uint32_t fromId, uint32_t toId, const std::string& description,
                   int colorR = -1, int colorG = -1, int colorB = -1,
                   double widthPx = 0, bool dashed = false);
```

保留原有无样式参数的 `AddCustomLink(fromId, toId, description)`，内部调用新重载并传入 -1/0/false 即可。

### 3.2 在 WriteLinkProperties 中传递样式（animation-interface.cc）

在写入 custom links 的循环里，把 `CustomLinkElement` 的样式赋给 `LinkProperties`：

```cpp
LinkProperties lp;
lp.fromNodeDescription = "";
lp.toNodeDescription = "";
lp.linkDescription = it->description;
if (it->colorR >= 0 && it->colorG >= 0 && it->colorB >= 0) {
    lp.linkColorR = it->colorR;
    lp.linkColorG = it->colorG;
    lp.linkColorB = it->colorB;
}
lp.linkWidth = it->widthPx;
lp.linkDashed = it->dashed;
m_linkProperties[p2pPair] = lp;
```

对 P2P 链路的 `LinkProperties` 若也要支持样式，需在 ns-3 里提供类似 `UpdateLinkDescription(from, to, desc, r, g, b, w, dashed)` 的 API，并在写入 P2P 链路时同样填充这些字段。

### 3.3 在 WriteXmlLink 中写出 XML 属性（animation-interface.cc）

在 `WriteXmlLink` 中，在现有 `fd`/`td`/`ld` 之后，按需写出可选属性（仅当已设置时写，保证向后兼容）：

```cpp
element.AddAttribute("fd", lprop.fromNodeDescription, true);
element.AddAttribute("td", lprop.toNodeDescription, true);
element.AddAttribute("ld", lprop.linkDescription, true);
if (lprop.linkColorR >= 0 && lprop.linkColorG >= 0 && lprop.linkColorB >= 0) {
    element.AddAttribute("lr", lprop.linkColorR);
    element.AddAttribute("lg", lprop.linkColorG);
    element.AddAttribute("lb", lprop.linkColorB);
}
if (lprop.linkWidth > 0)
    element.AddAttribute("lw", lprop.linkWidth);
if (lprop.linkDashed)
    element.AddAttribute("dashed", 1);
```

这样生成的 XML 中会出现例如：

- `<link fromId="0" toId="4" fd="..." td="..." ld="SPN-GMC-Backhaul" lr="255" lg="0" lb="0" lw="3" />`
- `<link fromId="5" toId="4" ... ld="Node-SPN-Data" lr="0" lg="0" lb="255" lw="1" dashed="1" />`

旧版 NetAnim 查看器不识别这些属性也不会报错，只是忽略；新版查看器可据此绘制不同样式。

### 3.4 在 scratch 中调用（heterogeneous-nms-framework.cc）

扩展完成后，可这样区分回程与数据链路：

```cpp
// SPN–GMC：红 255,0,0、实线、3px
m_anim->UpdateLinkDescription(gmcId, m_spnLte->GetId(), "SPN-GMC-Backhaul");
// 若已实现 UpdateLinkStyle 或带样式的 UpdateLinkDescription：
// m_anim->UpdateLinkStyle(gmcId, m_spnLte->GetId(), 255, 0, 0, 3.0, false);

m_anim->AddCustomLink(ue->GetId(), spnLteId, "Node-SPN-Data", 0, 0, 255, 1.0, true);
```

（若仅扩展了 `AddCustomLink` 的样式参数，则 P2P 回程链路的样式仍需通过扩展 `UpdateLinkDescription`/`UpdateLinkStyle` 并写入 `m_linkProperties` 实现。）

---

## 四、NetAnim 查看器侧：解析并绘制

### 4.1 获取 NetAnim 查看器源码

查看器**不在** ns-3 仓库内，需单独克隆（Mercurial）：

```bash
hg clone https://code.nsnam.org/netanim
cd netanim
# 或 NetAnim 2.0（若已迁移）：
# hg clone https://code.nsnam.org/jabraham3/netanim2
```

构建（需 Qt5）：

```bash
make clean
qmake NetAnim.pro   # 或 qmake-qt5 NetAnim.pro
make
./NetAnim
```

### 4.2 在查看器中要做的事

1. **解析 `<link>`**  
   找到读取 XML 的代码（通常为 Qt 的 `QXmlStreamReader` 或 `QDomDocument`），在解析到 `<link>` 时，除 `fromId`/`toId`/`fd`/`td`/`ld` 外，再读取可选属性：
   - `lr`, `lg`, `lb`（整数 0–255）→ 链路颜色
   - `lw`（浮点数）→ 线宽（像素或逻辑单位）
   - `dashed`（存在或为 1）→ 虚线

2. **存储到内部结构**  
   将每条链路的 (fromId, toId) 及样式 (r,g,b, width, dashed) 存到查看器自己的“链路”数据结构中（例如 `LinkItem` 或类似），便于绘制时使用。

3. **绘制时使用 QPen**  
   找到“根据节点位置画线段”的代码（通常用 `QGraphicsLineItem` 或 `QPainter::drawLine`），根据链路样式设置 `QPen`：
   - 颜色：`QColor(lr, lg, lb)`
   - 线宽：`setWidthF(lw)` 或 `setWidth((int)lw)`
   - 线型：`dashed ? Qt::DashLine : Qt::SolidLine`

示例（Qt 伪代码）：

```cpp
QPen pen;
if (link.hasCustomColor)
    pen.setColor(QColor(link.lr, link.lg, link.lb));
else
    pen.setColor(Qt::black);  // 默认
if (link.widthPx > 0)
    pen.setWidthF(link.widthPx);
pen.setStyle(link.dashed ? Qt::DashLine : Qt::SolidLine);
lineItem->setPen(pen);
```

4. **`<linkupdate>`**  
   若运行时通过 `linkupdate` 更新了 `ld`，且将来也要支持运行时改样式，需在解析 `linkupdate` 时同样解析可选的 `lr`/`lg`/`lb`/`lw`/`dashed`，并更新对应链路的样式再重绘。

---

## 五、验证流程

1. **ns-3**  
   - 编译：`./ns3 build heterogeneous-nms-main`
   - 运行仿真并生成 XML。  
   - 用文本编辑器或 `grep` 检查生成的 XML 中 `<link>` 是否包含 `lr`/`lg`/`lb`/`lw`/`dashed`。

2. **NetAnim 查看器**  
   - 用扩展后的 NetAnim 打开该 XML。  
   - 检查 SPN–GMC 是否为红色、较粗实线，Node–SPN 是否为蓝色、较细虚线。

3. **兼容性**  
   - 用**未修改**的旧版 NetAnim 打开同一 XML，应仍能正常加载并播放（仅连线样式为默认），说明扩展是向后兼容的。

---

## 六、小结

| 目标 | ns-3 侧 | NetAnim 查看器侧 |
|------|--------|-------------------|
| 连线颜色 | `LinkProperties` + `CustomLinkElement` 增加 r,g,b；`WriteXmlLink` 写出 `lr`/`lg`/`lb` | 解析 `lr`/`lg`/`lb`，`QPen::setColor` |
| 线宽 | 增加 `linkWidth`/`widthPx`，写出 `lw` | 解析 `lw`，`QPen::setWidthF` |
| 实线/虚线 | 增加 `linkDashed`/`dashed`，写出 `dashed` | 解析 `dashed`，`QPen::setStyle(DashLine/SolidLine)` |

实现时先完成 ns-3 的 XML 扩展与写入，再在 NetAnim 仓库中查找 `<link>` 的解析与“画线”代码，按上述属性驱动 `QPen` 即可。若 NetAnim 仓库迁移到 Git，可从 https://www.nsnam.org/wiki/NetAnim 或 https://www.nsnam.org/wiki/NetAnim2 获取当前克隆地址与构建说明。
