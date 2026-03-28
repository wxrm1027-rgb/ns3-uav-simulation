# Wireshark HNMP 解析器

- **脚本**：`hnmp_dissector.lua`
- **协议显示名**：`HNMP`
- **UDP 端口**：`8080`、`8888`、`9999`

## 安装

1. 打开 Wireshark → **帮助** → **关于 Wireshark** → **文件夹** → **Personal Lua Plugins**。
2. 将 `hnmp_dissector.lua` 复制到该目录（或软链接）。
3. 完全退出并重新启动 Wireshark。

若使用 **init.lua**，可在 Personal Lua 配置中加入：

```lua
dofile("/你的路径/ns-3-dev/docs/wireshark/hnmp_dissector.lua")
```

## 使用

- 显示过滤器示例：`udp.port == 8080 || udp.port == 8888 || udp.port == 9999`
- **8 字节 double**：脚本优先使用 Lua `struct` 模块的 `unpack(">d")`（与 Wireshark 捆绑提供）。若本机不可用，相关字段可能仅以原始字节显示。

## 与仿真代码的对应关系

- **6 字节头**：与 `Hnmp::EncodeFrame(..., implicitCompact3B=false)` 一致。
- **3 字节压缩头**：与窄带 `implicitCompact3B=true` 一致（仅 FrameType、QoS、PayloadLen）。
- 若一帧同时满足 6 字节与 3 字节长度自洽条件，**优先按 6 字节头**解析。
