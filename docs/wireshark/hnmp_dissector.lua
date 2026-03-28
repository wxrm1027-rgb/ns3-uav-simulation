--[[
  Wireshark Lua Dissector: HNMP (Heterogeneous Network Management Protocol)
  与 ns-3 仿真中 heterogeneous-nms-framework / hnmp 实现一致。

  用法：
    1) 帮助 -> 关于 Wireshark -> 文件夹 -> Personal Lua Plugins，将本脚本放入该目录；
       或编辑 Personal Lua -> init.lua，添加：dofile("hnmp_dissector.lua") 的完整路径。
    2) 重启 Wireshark，对 udp.port == 8080 || udp.port == 8888 || udp.port == 9999 抓包即可解析。

  说明：
    - 支持 6 字节完整头（Adhoc/LTE 等）与 3 字节隐式压缩头（窄带 DataLink，与 C++ implicitCompact3B 一致）。
    - 若同时满足两种长度关系，优先按 6 字节完整头解析。
    - 浮点：优先使用 Wireshark TvbRange 的 :float()（4B）；8B double 使用 struct.unpack(">d")（大端 IEEE754）。
]]

local hnmp = Proto("HNMP", "Heterogeneous Network Management Protocol")

-- 可选：部分 Wireshark 安装带 struct；若无则 double 显示为 hex
local struct_mod = nil
do
  local ok, m = pcall(require, "struct")
  if ok and m then
    struct_mod = m
  end
end

local hf = hnmp.fields

-- ========== HNMP Header ==========
hf.frame_type   = ProtoField.uint8("hnmp.frame_type", "Frame Type", base.HEX)
hf.qos_level    = ProtoField.uint8("hnmp.qos_level", "QoS Level", base.DEC)
hf.source_id    = ProtoField.uint8("hnmp.source_id", "Source ID", base.DEC)
hf.dest_id      = ProtoField.uint8("hnmp.dest_id", "Dest ID", base.DEC)
hf.sequence     = ProtoField.uint8("hnmp.sequence", "Sequence", base.DEC)
hf.payload_len  = ProtoField.uint8("hnmp.payload_len", "Payload Length", base.DEC)
-- ========== TLV 通用 ==========
hf.tlv_type     = ProtoField.uint8("hnmp.tlv.type", "TLV Type", base.HEX)
hf.tlv_length   = ProtoField.uint16("hnmp.tlv.length", "TLV Value Length", base.DEC)
hf.tlv_value    = ProtoField.bytes("hnmp.tlv.value", "TLV Value (raw)")

-- 常用子字段（按类型挂到子树）
hf.tlv_energy   = ProtoField.double("hnmp.tlv.energy", "Energy")
hf.tlv_linkq    = ProtoField.double("hnmp.tlv.link_quality", "Link Quality")
hf.tlv_mobility = ProtoField.double("hnmp.tlv.mobility", "Mobility")
hf.tlv_string   = ProtoField.string("hnmp.tlv.text", "ASCII Text")

hf.tlv_delta_e  = ProtoField.double("hnmp.tlv.delta_energy", "Delta Energy")

hf.tlv_node_id  = ProtoField.uint32("hnmp.tlv.node_id", "Node ID", base.DEC)
hf.tlv_score    = ProtoField.double("hnmp.tlv.score", "Score")

hf.tlv_px = ProtoField.double("hnmp.tlv.px", "Position X")
hf.tlv_py = ProtoField.double("hnmp.tlv.py", "Position Y")
hf.tlv_pz = ProtoField.double("hnmp.tlv.pz", "Position Z")
hf.tlv_vx = ProtoField.float("hnmp.tlv.vx", "Velocity X")
hf.tlv_vy = ProtoField.float("hnmp.tlv.vy", "Velocity Y")
hf.tlv_vz = ProtoField.float("hnmp.tlv.vz", "Velocity Z")
hf.tlv_neighbor_count = ProtoField.uint16("hnmp.tlv.neighbor_count", "Neighbor Count", base.DEC)
hf.tlv_neighbor_id    = ProtoField.uint32("hnmp.tlv.neighbor_id", "Neighbor ID", base.DEC)

hf.tlv_agg_node_count = ProtoField.uint16("hnmp.tlv.agg.node_written", "Node Report Count (written)", base.DEC)
hf.tlv_agg_edge_count = ProtoField.uint16("hnmp.tlv.agg.edge_count", "Edge Count", base.DEC)
hf.tlv_agg_raw        = ProtoField.bytes("hnmp.tlv.agg.raw", "Aggregate Remainder (raw)")
hf.tlv_edge_a         = ProtoField.uint32("hnmp.tlv.edge_a", "Edge Endpoint A", base.DEC)
hf.tlv_edge_b         = ProtoField.uint32("hnmp.tlv.edge_b", "Edge Endpoint B", base.DEC)

hf.tlv_sf_ttl   = ProtoField.uint8("hnmp.tlv.score_flood.ttl", "TTL", base.DEC)
hf.tlv_sf_n     = ProtoField.uint16("hnmp.tlv.score_flood.n", "Entry Count", base.DEC)

hf.tlv_hb_primary = ProtoField.uint32("hnmp.tlv.heartbeat.primary_id", "Primary SPN ID", base.DEC)
hf.tlv_hb_ts      = ProtoField.double("hnmp.tlv.heartbeat.ts", "Timestamp (sim s)")

-- ========== 常量 ==========
local FT_REQUEST  = 0x01
local FT_RESPONSE = 0x02
local FT_ALERT    = 0x03
local FT_REPORT   = 0x04
local FT_POLICY   = 0x05

local function frame_type_name(b)
  local t = {
    [FT_REQUEST]  = "REQUEST",
    [FT_RESPONSE] = "RESPONSE",
    [FT_ALERT]    = "ALERT",
    [FT_REPORT]   = "REPORT",
    [FT_POLICY]   = "POLICY",
  }
  return t[b] or string.format("0x%02X", b)
end

local function tlv_type_name(t)
  local n = {
    [0x10] = "Telemetry",
    [0x11] = "Role",
    [0x12] = "Config/Policy",
    [0x20] = "BusinessPerf",
    [0x30] = "Topology",
    [0x31] = "LinkCtrl",
    [0x40] = "Fault",
    [0x41] = "RouteFail",
    [0x80] = "HelloElection",
    [0x81] = "NodeReportSpn",
    [0x82] = "TopologyAggregate",
    [0x83] = "ScoreFlood",
    [0x84] = "HeartbeatSync",
  }
  return n[t] or string.format("0x%02X", t)
end

--- 大端 IEEE754 double（8B）
local function read_be_double(tvb, offset)
  if tvb:len() < offset + 8 then
    return nil
  end
  local r = tvb:range(offset, 8)
  if struct_mod then
    local ok, v = pcall(function()
      return struct_mod.unpack(">d", r:raw())
    end)
    if ok then
      return v
    end
  end
  return nil
end

--- 4B float：按用户要求使用 TvbRange:float()（网络序）
local function read_be_float(tvb, offset)
  if tvb:len() < offset + 4 then
    return nil
  end
  local ok, v = pcall(function()
    return tvb:range(offset, 4):float()
  end)
  if ok then
    return v
  end
  return nil
end

--- 与 C++ NodeReportSpn 体一致：返回下一记录起始绝对偏移；失败返回 nil
local function end_of_node_report(tvb, base, vmax)
  if base + 58 > vmax then
    return nil
  end
  local nc = tvb:range(base + 56, 2):uint()
  local reclen = 58 + nc * 4
  if base + reclen > vmax then
    return nil
  end
  return base + reclen
end

--- 解析单个 TLV，返回 (consumed_bytes, short_label)
local function dissect_tlv(tvb, pktinfo, tree, base_off)
  if tvb:len() < base_off + 3 then
    return 0, nil
  end
  local t = tvb:range(base_off, 1):uint()
  local vlen = tvb:range(base_off + 1, 2):uint()
  local total = 3 + vlen
  if tvb:len() < base_off + total then
    tree:add(hnmp, tvb:range(base_off, tvb:len() - base_off)):append_text(" [TLV truncated, need " .. total .. " bytes]")
    return tvb:len() - base_off, tlv_type_name(t)
  end

  local tlv_tree = tree:add(hnmp, tvb:range(base_off, total), string.format("TLV: %s (len=%u)", tlv_type_name(t), vlen))
  tlv_tree:add(hf.tlv_type, tvb:range(base_off, 1))
  tlv_tree:add(hf.tlv_length, tvb:range(base_off + 1, 2))
  local voff = base_off + 3
  local val_range = tvb:range(voff, vlen)

  local label = tlv_type_name(t)

  if t == 0x10 and vlen >= 24 then
    local e = read_be_double(tvb, voff)
    local lq = read_be_double(tvb, voff + 8)
    local mob = read_be_double(tvb, voff + 16)
    if e then tlv_tree:add(hf.tlv_energy, val_range:range(0, 8), e) else tlv_tree:add(hf.tlv_value, val_range:range(0, 8)):append_text(" (Energy raw)") end
    if lq then tlv_tree:add(hf.tlv_linkq, val_range:range(8, 8), lq) else tlv_tree:add(hf.tlv_value, val_range:range(8, 8)) end
    if mob then tlv_tree:add(hf.tlv_mobility, val_range:range(16, 8), mob) else tlv_tree:add(hf.tlv_value, val_range:range(16, 8)) end
    if vlen > 24 then
      tlv_tree:add(hf.tlv_value, val_range:range(24, vlen - 24))
    end
  elseif t == 0x12 then
    local ok, s = pcall(function()
      return val_range:string()
    end)
    if ok and s then
      tlv_tree:add(hf.tlv_string, val_range, s)
    else
      tlv_tree:add(hf.tlv_value, val_range)
    end
  elseif t == 0x31 and vlen >= 8 then
    local d = read_be_double(tvb, voff)
    if d then
      tlv_tree:add(hf.tlv_delta_e, val_range:range(0, 8), d)
    else
      tlv_tree:add(hf.tlv_value, val_range:range(0, 8))
    end
    if vlen > 8 then
      tlv_tree:add(hf.tlv_value, val_range:range(8, vlen - 8))
    end
  elseif t == 0x80 and vlen >= 12 then
    local nid = tvb:range(voff, 4):uint()
    tlv_tree:add(hf.tlv_node_id, tvb:range(voff, 4))
    local sc = read_be_double(tvb, voff + 4)
    if sc then
      tlv_tree:add(hf.tlv_score, val_range:range(4, 8), sc)
    else
      tlv_tree:add(hf.tlv_value, val_range:range(4, 8))
    end
    if vlen > 12 then
      tlv_tree:add(hf.tlv_value, val_range:range(12, vlen - 12))
    end
    label = string.format("Hello(n=%u)", nid)
  elseif t == 0x81 and vlen >= 4 + 24 + 12 + 8 + 8 + 2 then
    local o = voff
    tlv_tree:add(hf.tlv_node_id, tvb:range(o, 4))
    local nid = tvb:range(o, 4):uint()
    o = o + 4
    local px, py, pz = read_be_double(tvb, o), read_be_double(tvb, o + 8), read_be_double(tvb, o + 16)
    if px then tlv_tree:add(hf.tlv_px, val_range:range(o - voff, 8), px) end
    if py then tlv_tree:add(hf.tlv_py, val_range:range(o - voff + 8, 8), py) end
    if pz then tlv_tree:add(hf.tlv_pz, val_range:range(o - voff + 16, 8), pz) end
    o = o + 24
    local vx, vy, vz = read_be_float(tvb, o), read_be_float(tvb, o + 4), read_be_float(tvb, o + 8)
    if vx then tlv_tree:add(hf.tlv_vx, val_range:range(o - voff, 4), vx) end
    if vy then tlv_tree:add(hf.tlv_vy, val_range:range(o - voff + 4, 4), vy) end
    if vz then tlv_tree:add(hf.tlv_vz, val_range:range(o - voff + 8, 4), vz) end
    o = o + 12
    local en = read_be_double(tvb, o)
    local lq = read_be_double(tvb, o + 8)
    if en then tlv_tree:add(hf.tlv_energy, val_range:range(o - voff, 8), en) end
    if lq then tlv_tree:add(hf.tlv_linkq, val_range:range(o - voff + 8, 8), lq) end
    o = o + 16
    local nc = tvb:range(o, 2):uint()
    tlv_tree:add(hf.tlv_neighbor_count, tvb:range(o, 2))
    o = o + 2
    local need = nc * 4
    local remain = vlen - (o - voff)
    if need <= remain then
      for i = 0, nc - 1 do
        tlv_tree:add(hf.tlv_neighbor_id, tvb:range(o + i * 4, 4))
      end
      o = o + need
      if o < voff + vlen then
        tlv_tree:add(hf.tlv_value, tvb:range(o, voff + vlen - o))
      end
    else
      tlv_tree:add(hf.tlv_value, val_range:range(o - voff, remain)):append_text(" [NodeReportSpn: neighbor list truncated]")
    end
    label = string.format("ReportSPN(n=%u)", nid)
  elseif t == 0x82 and vlen >= 2 then
    -- C++ FlushAggregatedToGmc: Value = [n_written:2B][node reports...][n_edges:2B][edges 8B each]
    local n_written = tvb:range(voff, 2):uint()
    tlv_tree:add(hf.tlv_agg_node_count, tvb:range(voff, 2))
    local o = voff + 2
    local vmax = voff + vlen
    local agg_body
    if vlen > 2 then
      agg_body = tlv_tree:add(hnmp, val_range:range(2, vlen - 2), "Aggregate body")
    else
      agg_body = tlv_tree
    end
    for i = 1, n_written do
      if o >= vmax then
        break
      end
      local o_next = end_of_node_report(tvb, o, vmax)
      if not o_next then
        agg_body:add(hf.tlv_agg_raw, tvb:range(o, vmax - o)):append_text(" [truncated / malformed node record]")
        break
      end
      local nid = tvb:range(o, 4):uint()
      local nsub = agg_body:add(hnmp, tvb:range(o, o_next - o), string.format("Node #%u (id=%u)", i, nid))
      nsub:add(hf.tlv_node_id, tvb:range(o, 4))
      nsub:add(hf.tlv_value, tvb:range(o + 4, o_next - o - 4)):append_text(" (pos/vel/energy/neighbors — 同 0x81 布局)")
      o = o_next
    end
    if o + 2 <= vmax then
      local n_edges = tvb:range(o, 2):uint()
      tlv_tree:add(hf.tlv_agg_edge_count, tvb:range(o, 2))
      o = o + 2
      for e = 1, n_edges do
        if o + 8 > vmax then
          agg_body:add(hf.tlv_agg_raw, tvb:range(o, vmax - o)):append_text(" [edges truncated]")
          break
        end
        local esub = agg_body:add(hnmp, tvb:range(o, 8), string.format("Edge #%u", e))
        esub:add(hf.tlv_edge_a, tvb:range(o, 4))
        esub:add(hf.tlv_edge_b, tvb:range(o + 4, 4))
        o = o + 8
      end
    end
    if o < vmax then
      agg_body:add(hf.tlv_agg_raw, tvb:range(o, vmax - o))
    end
    label = string.format("TopoAgg(n=%u)", n_written)
  elseif t == 0x83 and vlen >= 1 + 2 then
    local ttl = tvb:range(voff, 1):uint()
    local n = tvb:range(voff + 1, 2):uint()
    tlv_tree:add(hf.tlv_sf_ttl, tvb:range(voff, 1))
    tlv_tree:add(hf.tlv_sf_n, tvb:range(voff + 1, 2))
    local o = voff + 3
    local i = 0
    while i < n and o + 12 <= voff + vlen and o + 12 <= tvb:len() do
      tlv_tree:add(hf.tlv_node_id, tvb:range(o, 4))
      local sc = read_be_double(tvb, o + 4)
      if sc then
        tlv_tree:add(hf.tlv_score, tvb:range(o + 4, 8), sc)
      else
        tlv_tree:add(hf.tlv_value, tvb:range(o + 4, 8))
      end
      o = o + 12
      i = i + 1
    end
    if o < voff + vlen then
      tlv_tree:add(hf.tlv_value, val_range:range(o - voff, vlen - (o - voff)))
    end
    label = string.format("ScoreFlood(n=%u)", n)
  elseif t == 0x84 and vlen >= 12 then
    tlv_tree:add(hf.tlv_hb_primary, tvb:range(voff, 4))
    local ts = read_be_double(tvb, voff + 4)
    if ts then
      tlv_tree:add(hf.tlv_hb_ts, val_range:range(4, 8), ts)
    else
      tlv_tree:add(hf.tlv_value, val_range:range(4, 8))
    end
    if vlen > 12 then
      tlv_tree:add(hf.tlv_value, val_range:range(12, vlen - 12))
    end
    label = "Heartbeat"
  else
    tlv_tree:add(hf.tlv_value, val_range)
  end

  return total, label
end

function hnmp.dissector(tvb, pktinfo, root)
  local len = tvb:len()
  if len < 3 then
    return
  end

  pktinfo.cols.protocol:set("HNMP")

  local hdr_len = 0
  local plen = 0
  local compact = false

  local full_ok = (len >= 6) and (len == 6 + tvb:range(5, 1):uint())
  local compact_ok = (len == 3 + tvb:range(2, 1):uint())

  if full_ok then
    hdr_len = 6
    plen = tvb:range(5, 1):uint()
    compact = false
  elseif compact_ok then
    hdr_len = 3
    plen = tvb:range(2, 1):uint()
    compact = true
  elseif len >= 6 then
    -- 长度不匹配时仍尝试按 6 字节头展示（便于排错）
    hdr_len = 6
    plen = tvb:range(5, 1):uint()
    compact = false
  else
    hdr_len = 3
    plen = tvb:range(2, 1):uint()
    compact = true
  end

  local subtree = root:add(hnmp, tvb:range(0, len), "HNMP")
  if not full_ok and not compact_ok and len >= 6 and len < 6 + plen then
    subtree:append_text(" [WARN: declared payload length exceeds packet]")
  end
  local hdr_tree = subtree:add(hnmp, tvb:range(0, hdr_len), "HNMP Header")

  local ft = tvb:range(0, 1):uint()
  hdr_tree:add(hf.frame_type, tvb:range(0, 1))
  hdr_tree:add(hf.qos_level, tvb:range(1, 1))
  if compact then
    hdr_tree:append_text(" [Compact 3-byte header]")
    hdr_tree:add(hf.payload_len, tvb:range(2, 1))
  else
    hdr_tree:append_text(" [Full 6-byte header]")
    hdr_tree:add(hf.source_id, tvb:range(2, 1))
    hdr_tree:add(hf.dest_id, tvb:range(3, 1))
    hdr_tree:add(hf.sequence, tvb:range(4, 1))
    hdr_tree:add(hf.payload_len, tvb:range(5, 1))
  end

  local pay_off = hdr_len
  local pay_len = math.min(plen, len - pay_off)
  if pay_len < 0 then
    pay_len = 0
  end

  local tlv_labels = {}
  local pos = pay_off
  local end_pos = pay_off + pay_len
  while pos < end_pos and pos + 3 <= len do
    local consumed, lab = dissect_tlv(tvb, pktinfo, subtree, pos)
    if consumed <= 0 then
      break
    end
    if lab then
      tlv_labels[#tlv_labels + 1] = lab
    end
    pos = pos + consumed
  end
  if pos < end_pos then
    subtree:add(hf.tlv_value, tvb:range(pos, end_pos - pos)):append_text(" (unparsed trailing)")
  end

  local info = string.format("HNMP [%s]", frame_type_name(ft))
  if #tlv_labels > 0 then
    info = info .. " [" .. table.concat(tlv_labels, ", ") .. "]"
  end
  pktinfo.cols.info:append(" | " .. info)
end

-- 注册 UDP 端口（与仿真脚本一致：8080 数据面；8888/9999 双通道控制/数据）
local udp_table = DissectorTable.get("udp.port")
udp_table:add(8080, hnmp)
udp_table:add(8888, hnmp)
udp_table:add(9999, hnmp)
