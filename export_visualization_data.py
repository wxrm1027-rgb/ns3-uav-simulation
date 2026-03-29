#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从仿真结果目录导出可视化平台所需数据：topology.json（含 adhocCluster 簇首历史）、events.json。
可选用 NetAnim XML 或 log 解析拓扑与事件；flowmon 统计由前端或 plot-flowmon 使用。

用法:
  python3 export_visualization_data.py simulation_results/2026-03-08_14-03-20
  python3 export_visualization_data.py simulation_results/2026-03-08_14-03-20 -o ./visualization/data
"""

import argparse
import json
import os
import random
import re
import xml.etree.ElementTree as ET


def parse_int(s, default=0):
    if s is None:
        return default
    try:
        return int(re.sub(r"[^0-9\-]", "", s))
    except ValueError:
        return default


def parse_float(s, default=0.0):
    if s is None:
        return default
    try:
        return float(re.sub(r"[^0-9eE.\-+]", "", s))
    except ValueError:
        return default


def parse_nms_log_line(line):
    """解析 NmsLog 格式: [1.234s] [Node 0] [EVENT_TYPE] details"""
    m = re.match(r"\[\s*([\d.]+)\s*s\s*\]\s*\[\s*Node\s+(\d+)\s*\]\s*\[\s*(\S+)\s*\]\s*(.*)", line.strip())
    if not m:
        return None
    t = float(m.group(1))
    node_id = int(m.group(2))
    event_type = m.group(3)
    details = m.group(4).strip()
    return {"t": t, "nodeId": node_id, "event": event_type, "details": details}


def load_events_from_log(log_path):
    events = []
    if not os.path.isfile(log_path):
        return events
    with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            if not line.strip() or line.startswith("="):
                continue
            ev = parse_nms_log_line(line)
            if ev:
                events.append(ev)
    events.sort(key=lambda e: (e["t"], e["nodeId"]))
    return events


def infer_node_role_from_events(events):
    """从事件中推断节点角色与子网。Adhoc/DataLink 主/备由 SPN_ANNOUNCE（与仿真 C++ 一致）决定，其余为 TSN。"""
    role_by_node = {}
    subnet_by_node = {}
    # 第一遍：确定 GMC、LTE 角色及所有节点的子网
    for ev in events:
        n = ev["nodeId"]
        et = ev["event"]
        d = ev.get("details", "")
        if et == "SYSTEM" and n == 0:
            role_by_node[0] = "gmc"
            subnet_by_node[0] = "gmc"
        if et == "NODE_ONLINE":
            if "GMC" in d or n == 0:
                role_by_node[n] = "gmc"
                subnet_by_node[n] = "gmc"
            elif "LTE SPN" in d or "eNB" in d:
                role_by_node[n] = "spn"
                subnet_by_node[n] = "lte"
            elif "LTE UE" in d:
                role_by_node[n] = "ue"
                subnet_by_node[n] = "lte"
            elif "Adhoc" in d or "Adhoc-SPN" in d:
                subnet_by_node[n] = "adhoc"
            elif "DL-SPN" in d or "DataLink" in d:
                subnet_by_node[n] = "datalink"

    # 按时间顺序更新：最新一条 SPN_ANNOUNCE 中 primary/backup（subnet=1 Adhoc, 2 DataLink）
    announce_pri = {}
    announce_bak = {}
    for ev in events:
        if ev["event"] != "SPN_ANNOUNCE":
            continue
        d = ev.get("details", "")
        ms = re.search(r"subnet=(\d+)", d)
        mp = re.search(r"primary=(\d+)", d)
        mb = re.search(r"backup=(\d+)", d)
        if not ms or not mp or not mb:
            continue
        sk = ms.group(1)
        announce_pri[sk] = int(mp.group(1))
        announce_bak[sk] = int(mb.group(1))

    for nid, sub in list(subnet_by_node.items()):
        sl = (sub or "").lower()
        if sl == "gmc":
            continue
        if sl == "lte":
            sk = "0"
            p = announce_pri.get(sk)
            if p is not None and nid == p:
                role_by_node[nid] = "PRIMARY_SPN"
            elif nid not in role_by_node:
                role_by_node[nid] = "TSN"
            continue
        if nid in role_by_node:
            continue
        if sl == "adhoc":
            sk = "1"
        elif sl == "datalink":
            sk = "2"
        else:
            role_by_node[nid] = "node"
            continue
        p = announce_pri.get(sk)
        b = announce_bak.get(sk)
        if p is not None and nid == p:
            role_by_node[nid] = "PRIMARY_SPN"
        elif b is not None and nid == b and p != nid:
            role_by_node[nid] = "BACKUP_SPN"
        else:
            role_by_node[nid] = "TSN"

    return role_by_node, subnet_by_node


def role_is_spn_for_backhaul(r):
    """回程边判定：兼容旧 spn / 新 PRIMARY_SPN。"""
    if not r:
        return False
    return r in ("spn", "PRIMARY_SPN", "primary_spn")


def get_join_time_by_node(events):
    """从 NODE_ONLINE / NODE_UP 与 SYSTEM 得到每个节点的入网时间（与仿真日志一致）。"""
    join_time = {}
    for ev in events:
        n = ev["nodeId"]
        t = ev["t"]
        if ev["event"] == "SYSTEM" and n == 0:
            join_time[0] = min(join_time.get(0, t), t)
        if ev["event"] in ("NODE_ONLINE", "NODE_UP"):
            join_time[n] = min(join_time.get(n, t), t)
    return join_time


def get_offline_time_by_node(events):
    """从 NODE_OFFLINE 得到每个节点的退网时间（最后一次发生的时间）。"""
    offline_time = {}
    for ev in events:
        if ev["event"] != "NODE_OFFLINE":
            continue
        n = ev["nodeId"]
        t = ev["t"]
        offline_time[n] = max(offline_time.get(n, t), t)
    return offline_time


def extract_adhoc_ch_elect_history(events):
    """从 CH_ANNOUNCE 解析 Adhoc 簇首选举历史（subnet=1，与仿真 C++ 日志一致）。"""
    hist = []
    for ev in events or []:
        if ev.get("event") != "CH_ANNOUNCE":
            continue
        d = ev.get("details") or ""
        m_sub = re.search(r"subnet\s*=\s*(\d+)", d, re.I)
        if m_sub and m_sub.group(1) != "1":
            continue
        m_ch = re.search(r"cluster_head\s*=\s*(\d+)", d, re.I)
        if not m_ch:
            continue
        ch = int(m_ch.group(1))
        reason = "unknown"
        m_re = re.search(r"\breason\s*=\s*(\S+)", d, re.I)
        if m_re:
            reason = m_re.group(1).strip()
        try:
            t = float(ev.get("t", 0))
        except (TypeError, ValueError):
            t = 0.0
        hist.append({"time": t, "ch": ch, "reason": reason})
    hist.sort(key=lambda x: (x["time"], x["ch"]))
    return hist


def get_power_off_time_from_jsonl(jsonl_records, energy_threshold=0.01):
    """从 JSONL 得到每个节点首次能量 <= threshold 的时间（视为没电）。"""
    power_off = {}
    for r in jsonl_records:
        nid = r["nodeId"]
        t = r["t"]
        energy = r.get("energy")
        if energy is None:
            continue
        try:
            e = float(energy)
        except (TypeError, ValueError):
            continue
        if e <= energy_threshold:
            if nid not in power_off:
                power_off[nid] = t
    return power_off


def get_join_time_from_jsonl(jsonl_records):
    """从 JSONL 得到每个节点首次 join_state=='joined' 的时间，用于补全入网时刻（无 NODE_ONLINE 时）。"""
    join_time = {}
    for r in jsonl_records:
        if r.get("join_state") != "joined":
            continue
        nid = r["nodeId"]
        t = r["t"]
        if nid not in join_time or t < join_time[nid]:
            join_time[nid] = t
    return join_time


def load_join_config(path):
    """解析 join_config.json，返回节点列表 [{"node_id", "subnet", "join_time", ...}]。"""
    if not path or not os.path.isfile(path):
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            raw = json.load(f)
    except (json.JSONDecodeError, OSError):
        return []
    if not isinstance(raw, list):
        return []
    out = []
    for item in raw:
        if not isinstance(item, dict):
            continue
        nid = item.get("node_id", item.get("nodeId"))
        sub = (item.get("subnet") or "").strip()
        jt = float(item.get("join_time", 0))
        rec = {"node_id": int(nid), "subnet": sub, "join_time": jt}
        if "neighbors" in item and isinstance(item["neighbors"], list):
            rec["neighbors"] = [int(x) for x in item["neighbors"] if isinstance(x, (int, float))]
        if "init_pos" in item and isinstance(item["init_pos"], (list, tuple)) and len(item["init_pos"]) >= 2:
            rec["init_pos"] = [float(item["init_pos"][0]), float(item["init_pos"][1]), float(item["init_pos"][2]) if len(item["init_pos"]) > 2 else 0.0]
        if "type" in item and isinstance(item["type"], str):
            rec["type"] = item["type"].strip()
        if "ip" in item:
            ip = str(item.get("ip", "")).strip()
            if ip:
                rec["ip"] = ip
        if "speed" in item and item["speed"] is not None:
            try:
                rec["speed"] = float(item["speed"])
            except (TypeError, ValueError):
                pass
        out.append(rec)
    return out


def build_intra_cluster_links_from_join_config(join_config, join_time):
    """根据 join_config 建立簇内物理连线：仅使用各节点的 neighbors 字段（与 C++ Matrix 一致），不再子网内两两全连。"""
    if not join_config:
        return []
    links = []
    seen = set()
    for n in join_config:
        nid = n["node_id"]
        sub = (n.get("subnet") or "").strip().lower()
        if not sub or sub == "gmc":
            continue
        for nb in n.get("neighbors") or []:
            if not isinstance(nb, (int, float)):
                continue
            nb_id = int(nb)
            if nb_id == nid:
                continue
            key = (min(nid, nb_id), max(nid, nb_id))
            if key in seen:
                continue
            seen.add(key)
            t = max(join_time.get(nid, 0), join_time.get(nb_id, 0))
            links.append({"from": key[0], "to": key[1], "type": "data", "t": t})
    return links


def dedup_links(links_raw, role_by_node, join_time):
    """链路去重与分层规则：(from,to) 归一化为 (min,max)。
    严格规则：只允许角色为 spn 的节点与 GMC(0) 建立回程；若一端为 GMC、另一端为普通 node，强制删除该链路。
    并为最终为 spn 的节点补全到 GMC 的回程边（若 XML 未包含）。"""
    seen = set()
    result = []
    gmc_id = 0
    for link in links_raw:
        f, t = link["from"], link["to"]
        key = (min(f, t), max(f, t))
        if key in seen:
            continue
        # 强制过滤：一端 GMC、另一端普通 node 的链路必须删除（不允许普通节点直连 GMC）
        if f == gmc_id or t == gmc_id:
            other = t if f == gmc_id else f
            if not role_is_spn_for_backhaul(role_by_node.get(other, "")):
                continue  # 删除 GMC-node 链路
        seen.add(key)
        from_role = role_by_node.get(f, "")
        to_role = role_by_node.get(t, "")
        is_backhaul = (
            (f == gmc_id and role_is_spn_for_backhaul(to_role))
            or (t == gmc_id and role_is_spn_for_backhaul(from_role))
        )
        link_t = max(join_time.get(f, 0), join_time.get(t, 0))
        result.append({
            "from": key[0], "to": key[1],
            "type": "backhaul" if is_backhaul else "data",
            "t": link_t,
        })
    # 为最终角色为 spn 的节点补全到 GMC 的回程边（C++ 不再为 Adhoc/DataLink 画 GMC 连线，需由脚本生成）
    for nid, role in role_by_node.items():
        if nid == gmc_id or not role_is_spn_for_backhaul(role):
            continue
        key = (min(gmc_id, nid), max(gmc_id, nid))
        if key not in seen:
            seen.add(key)
            result.append({
                "from": gmc_id, "to": nid,
                "type": "backhaul",
                "t": join_time.get(nid, 0),
            })
    return result


def load_topology_from_netanim_xml(xml_path):
    """从 NetAnim XML 提取节点位置（支持多时刻 t）与链路。"""
    node_pos = {}  # nid -> (x, y) 最后一帧
    node_positions = {}  # nid -> [(t, x, y), ...] 若存在带 t 的 p
    links_raw = []
    if not os.path.isfile(xml_path):
        return node_pos, node_positions, links_raw
    try:
        tree = ET.parse(xml_path)
        root = tree.getroot()
    except Exception:
        return node_pos, node_positions, links_raw
    for elem in root.iter():
        if elem.tag == "n":
            nid = parse_int(elem.get("id"), -1)
            x, y = parse_float(elem.get("locX")), parse_float(elem.get("locY"))
            if nid >= 0:
                node_pos[nid] = (x, y)
        elif elem.tag == "p" and elem.get("p") == "p":
            nid = parse_int(elem.get("id"), -1)
            x, y = parse_float(elem.get("x")), parse_float(elem.get("y"))
            t = parse_float(elem.get("t"), 0.0)
            if nid >= 0:
                node_pos[nid] = (x, y)
                if nid not in node_positions:
                    node_positions[nid] = []
                node_positions[nid].append((t, x, y))
        elif elem.tag == "link":
            f = parse_int(elem.get("fromId"), -1)
            t = parse_int(elem.get("toId"), -1)
            ld = elem.get("ld") or ""
            if f >= 0 and t >= 0:
                link_type = "backhaul" if "Backhaul" in ld or "SPN-GMC" in ld else "data"
                links_raw.append({"from": f, "to": t, "type": link_type})
    for nid in node_positions:
        node_positions[nid].sort(key=lambda v: v[0])
    return node_pos, node_positions, links_raw


def load_flowmon_stats(flowmon_xml_path):
    """解析 FlowMonitor XML，返回 flows 列表（throughput_mbps, avg_delay_ms, loss_pct 等）。"""
    flows = []
    if not flowmon_xml_path or not os.path.isfile(flowmon_xml_path):
        return flows
    try:
        tree = ET.parse(flowmon_xml_path)
        root = tree.getroot()
    except Exception:
        return flows
    for flow in root.findall(".//FlowStats/Flow"):
        fid = parse_int(flow.get("flowId"), 0)
        rx_packets = parse_int(flow.get("rxPackets"), 0)
        tx_packets = parse_int(flow.get("txPackets"), 1)
        rx_bytes = parse_int(flow.get("rxBytes"), 0)
        lost_packets = parse_int(flow.get("lostPackets"), 0)
        time_first_tx = parse_float(re.sub(r"ns", "", flow.get("timeFirstTxPacket", "0"), flags=re.I)) * 1e-9
        time_last_rx = parse_float(re.sub(r"ns", "", flow.get("timeLastRxPacket", "0"), flags=re.I)) * 1e-9
        duration = max(time_last_rx - time_first_tx, 1e-9)
        throughput_bps = rx_bytes * 8.0 / duration
        delay_sum_ns = parse_float(re.sub(r"ns", "", flow.get("delaySum", "0"), flags=re.I))
        avg_delay_s = (delay_sum_ns * 1e-9) / rx_packets if rx_packets > 0 else 0.0
        loss_pct = (lost_packets / tx_packets * 100.0) if tx_packets > 0 else 0.0
        loss_pct = min(loss_pct, 100.0)
        flows.append({
            "flowId": fid,
            "throughput_mbps": round(throughput_bps / 1e6, 4),
            "avg_delay_ms": round(avg_delay_s * 1e3, 2),
            "loss_pct": round(loss_pct, 2),
            "txPackets": tx_packets,
            "rxPackets": rx_packets,
            "lostPackets": lost_packets,
        })
    flows.sort(key=lambda f: f["flowId"])
    return flows


def extract_time_series_from_events(events):
    """从 PERF / STATE_CHANGE / PROXY_SWITCH / SPN_ELECT 提取能量、链路质量、移动性、Score 时序及 SPN 选举次数。"""
    energy = {}   # nodeId -> [(t, val)]
    link_quality = {}
    mobility = {}
    score = {}    # nodeId -> [(t, val)]
    spn_elect_count = 0
    spn_elect_by_subnet = {}
    for ev in events:
        n = ev["nodeId"]
        t = ev["t"]
        et = ev["event"]
        d = ev.get("details", "")
        if et == "SPN_ELECT":
            spn_elect_count += 1
            sub = "other"
            if "Subnet=LTE" in d:
                sub = "lte"
            elif "Subnet=Adhoc" in d:
                sub = "adhoc"
            elif "Subnet=DataLink" in d:
                sub = "datalink"
            spn_elect_by_subnet[sub] = spn_elect_by_subnet.get(sub, 0) + 1
            m = re.search(r"Score=([\d.]+)", d)
            if m and n not in (None, ""):
                if n not in score:
                    score[n] = []
                score[n].append((t, float(m.group(1))))
        if et == "PROXY_SWITCH":
            m = re.search(r"New:\s*([\d.]+)", d)
            if m:
                if n not in score:
                    score[n] = []
                score[n].append((t, float(m.group(1))))
        if et == "STATE_CHANGE":
            m = re.search(r"linkQ:\s*[\d.eE+\-]+\s*->\s*([\d.eE+\-]+)", d, re.I)
            if m and n not in (None, ""):
                if n not in link_quality:
                    link_quality[n] = []
                link_quality[n].append((t, float(m.group(1))))
        if et == "PERF":
            m = re.search(r"energy[=:]?\s*([\d.]+)", d, re.I)
            if m and n not in (None, ""):
                if n not in energy:
                    energy[n] = []
                energy[n].append((t, float(m.group(1))))
            m = re.search(r"linkQ[=:]?\s*([\d.]+)|link.?quality[=:]?\s*([\d.]+)", d, re.I)
            if m:
                v = m.group(1) or m.group(2)
                if v and n not in (None, ""):
                    if n not in link_quality:
                        link_quality[n] = []
                    link_quality[n].append((t, float(v)))
            m = re.search(r"mobility[=:]?\s*([\d.]+)", d, re.I)
            if m and n not in (None, ""):
                if n not in mobility:
                    mobility[n] = []
                mobility[n].append((t, float(m.group(1))))
    return {
        "energy": energy,
        "linkQuality": link_quality,
        "mobility": mobility,
        "score": score,
        "spnElectionCount": spn_elect_count,
        "spnElectionBySubnet": spn_elect_by_subnet,
    }


def extract_business_flows_from_events(events):
    """从事件中解析业务流路由与性能（FLOW_START/ROUTE_START 含 path/src/dst/priority/size/rate/type；FLOW_PERF 含 delay、throughput、loss）。"""
    flows = []
    flow_start = {}
    flow_perf = {}  # flowId -> {delayMs, throughputMbps, lossRate}
    flow_perf_main_seq = []  # FLOW_PERF_MAIN 顺序指标（FlowMonitor flowId 与业务 flow_id 不同，按启动顺序回填）
    for ev in events:
        et = ev.get("event", "")
        d = ev.get("details", "")
        t = ev.get("t", 0)
        if et == "ROUTE_START" or et == "FLOW_START":
            m = re.search(r"flowId[=:]?\s*(\d+)", d, re.I)
            fid = int(m.group(1)) if m else len(flow_start) + 1
            m = re.search(r"path[=:]?\s*([\d,\s]+)", d, re.I)
            path = []
            if m:
                path = [parse_int(x, -1) for x in re.split(r"[,;\s]+", m.group(1).strip()) if parse_int(x, -1) >= 0]
            m = re.search(r"type[=:]?\s*(\w+)", d, re.I)
            ftype = m.group(1) if m else "data"
            m = re.search(r"priority[=:]?\s*(\d+)", d, re.I)
            priority = int(m.group(1)) if m else 1
            m = re.search(r"src[=:]?\s*(\d+)", d, re.I)
            src = int(m.group(1)) if m else (path[0] if path else 0)
            m = re.search(r"dst[=:]?\s*(\d+)", d, re.I)
            dst = int(m.group(1)) if m else (path[-1] if path else 0)
            m = re.search(r"size[=:]?\s*(\d+)", d, re.I)
            size = int(m.group(1)) if m else 0
            m = re.search(r"rate[=:]?\s*([\d.]+)\s*Mbps", d, re.I)
            rate_mbps = parse_float(m.group(1), 0) if m else 0.0
            mhz = re.search(r"rate[=:]?\s*([\d.]+)\s*HZ", d, re.I)
            rate_hz = parse_float(mhz.group(1), 0) if mhz else None
            if et == "ROUTE_START" and fid in flow_start:
                # ROUTE_START is a route update event; do not overwrite flow metadata/time.
                if path and len(path) >= 2:
                    old_path = flow_start[fid].get("path", [])
                    if not old_path or len(old_path) < 2 or len(path) >= len(old_path):
                        flow_start[fid]["path"] = path
            else:
                flow_start[fid] = {
                    "id": fid, "type": ftype, "path": path, "startTime": t,
                    "priority": priority, "src": src, "dst": dst, "size": size, "rateMbps": rate_mbps,
                }
                if rate_hz is not None:
                    flow_start[fid]["rateHz"] = rate_hz
        elif et == "FLOW_PERF" or et == "FLOW_PERF_MAIN":
            m = re.search(r"flowId[=:]?\s*(\d+)", d, re.I)
            fid = int(m.group(1)) if m else None
            dm = re.search(r"delay[=:]?\s*([\d.]+)\s*ms", d, re.I)
            thr = re.search(r"throughput[=:]?\s*([\d.]+)\s*Mbps", d, re.I)
            loss = re.search(r"loss[=:]?\s*([\d.]+)\s*%", d, re.I)
            perf_entry = {
                "delayMs": parse_float(dm.group(1), 0) if dm else None,
                "throughputMbps": parse_float(thr.group(1), 0) if thr else None,
                "lossRate": (parse_float(loss.group(1), 0) / 100.0) if loss else None,
            }
            if et == "FLOW_PERF_MAIN":
                flow_perf_main_seq.append(perf_entry)
            elif fid is not None:
                flow_perf[fid] = perf_entry
        elif et == "ROUTE_END" or et == "FLOW_END":
            m = re.search(r"flowId[=:]?\s*(\d+)", d, re.I)
            fid = int(m.group(1)) if m else None
            if fid is not None and fid in flow_start:
                flow_start[fid]["endTime"] = t
                flow_start[fid]["label"] = flow_start[fid].get("type", "Flow") + "-" + str(fid)
                if fid in flow_perf:
                    flow_start[fid]["delayMs"] = flow_perf[fid].get("delayMs")
                    flow_start[fid]["throughputMbps"] = flow_perf[fid].get("throughputMbps")
                    flow_start[fid]["lossRate"] = flow_perf[fid].get("lossRate")
                flows.append(flow_start[fid])
                del flow_start[fid]
    for fid, f in flow_start.items():
        f["endTime"] = f.get("endTime", f["startTime"] + 100)
        f["label"] = f.get("type", "Flow") + "-" + str(fid)
        if fid in flow_perf:
            f["delayMs"] = flow_perf[fid].get("delayMs")
            f["throughputMbps"] = flow_perf[fid].get("throughputMbps")
            f["lossRate"] = flow_perf[fid].get("lossRate")
        flows.append(f)
    if flow_perf_main_seq and flows:
        flows_sorted = sorted(flows, key=lambda x: x.get("startTime", 0))
        for i, perf_item in enumerate(flow_perf_main_seq):
            if i >= len(flows_sorted):
                break
            if flows_sorted[i].get("delayMs") is None:
                flows_sorted[i]["delayMs"] = perf_item.get("delayMs")
            if flows_sorted[i].get("throughputMbps") is None:
                flows_sorted[i]["throughputMbps"] = perf_item.get("throughputMbps")
            if flows_sorted[i].get("lossRate") is None:
                flows_sorted[i]["lossRate"] = perf_item.get("lossRate")
    return flows


def build_business_features(business_flows):
    """从 business_flows 生成 businessFeatures 数组，供前端业务特征表格与拓扑高亮。字段：type, priority, src, dst, size, rateMbps, path。"""
    out = []
    for f in business_flows:
        feat = {
            "flowId": f.get("id", 0),
            "type": f.get("type", "data"),
            "priority": f.get("priority", 1),
            "src": f.get("src", 0),
            "dst": f.get("dst", 0),
            "size": f.get("size", 0),
            "rateMbps": f.get("rateMbps", 0.0),
            "path": f.get("path", []),
            "label": f.get("label", "Flow-" + str(f.get("id", 0))),
        }
        if not feat["path"] or len(feat["path"]) < 2:
            if feat["src"] and feat["dst"]:
                feat["path"] = [feat["src"], feat["dst"]]
        if f.get("rateHz") is not None:
            feat["rateHz"] = f["rateHz"]
        out.append(feat)
    return out


def extract_flow_perf_window_series(events):
    """解析 FLOW_PERF_WIN 事件，返回 bizFlowId -> [{t, delay, lossRate, throughput}]。"""
    out = {}
    for ev in events:
        if str(ev.get("event", "")).upper() != "FLOW_PERF_WIN":
            continue
        d = str(ev.get("details", ""))
        t = float(ev.get("t", 0) or 0)
        m_biz = re.search(r"bizFlowId[=:]?\s*(\d+)", d, re.I)
        if not m_biz:
            continue
        biz_id = parse_int(m_biz.group(1), 0)
        dm = re.search(r"delay[=:]?\s*([\d.]+)\s*ms", d, re.I)
        thr = re.search(r"throughput[=:]?\s*([\d.]+)\s*Mbps", d, re.I)
        loss = re.search(r"loss[=:]?\s*([\d.]+)\s*%", d, re.I)
        p = {
            "t": t,
            "delay": parse_float(dm.group(1), 0) if dm else None,
            "throughput": parse_float(thr.group(1), 0) if thr else 0.0,
            "lossRate": (parse_float(loss.group(1), 0) / 100.0) if loss else None,
        }
        out.setdefault(biz_id, []).append(p)
    for fid in list(out.keys()):
        out[fid] = sorted(out[fid], key=lambda x: x.get("t", 0))
    return out


def build_flow_performance_timeseries(business_flows, flow_perf_windows=None, window_sec=0.5):
    """从 businessFlows 生成 flowPerformance 时间序列。

    口径说明：
    - FlowMonitor 给出的 delay/loss/throughput 是该业务流在 [start, end] 区间上的汇总指标，而不是单点瞬时值。
    - 这里按固定窗口展开（默认 0.5s）为阶跃序列：
      start 前为 0/None，start 后立即进入稳定值，发送区间每个窗口输出一个点，end 后回到 0/None。
    """
    out = []
    ws = max(float(window_sec), 1e-3)
    for f in business_flows:
        fid = f.get("id", 0)
        start_t = f.get("startTime", 0)
        end_t = f.get("endTime", start_t + 100)
        delay_ms = f.get("delayMs")
        thr_mbps = f.get("throughputMbps")
        loss_rate = f.get("lossRate")  # 0~1
        win_points = (flow_perf_windows or {}).get(fid, [])
        if win_points:
            out.append({
                "flowId": fid,
                "label": f.get("label", "Flow-" + str(fid)),
                "path": f.get("path", []),
                "windowSec": ws,
                "data": win_points,
            })
            continue
        eps = 1e-3
        stable_thr = thr_mbps if thr_mbps is not None else 0.0
        data = []
        # start 前：未开始发送
        data.append({"t": max(0.0, start_t - eps), "delay": None, "lossRate": None, "throughput": 0.0})
        # 发送区间：每个窗口一个点，便于前端做“随时间变化”的观感
        cur = start_t
        while cur <= end_t + 1e-9:
            data.append({"t": round(cur, 3), "delay": delay_ms, "lossRate": loss_rate, "throughput": stable_thr})
            cur += ws
        # end 后：业务停止
        data.append({"t": round(end_t + eps, 3), "delay": None, "lossRate": None, "throughput": 0.0})
        out.append({
            "flowId": fid,
            "label": f.get("label", "Flow-" + str(fid)),
            "path": f.get("path", []),
            "windowSec": ws,
            "data": data,
        })
    return out


def _clamp01(v):
    return max(0.0, min(1.0, float(v)))


def build_network_radar_metrics(flows, kpi_summary, self_heal_times):
    """
    论文原始指标雷达图（0~1 归一化）：
    - throughput_norm（正向）
    - self_heal_speed_norm（正向，越快越好）
    - delay_norm（反向）
    - loss_norm（反向）
    - signaling_overhead_norm（反向）
    每个指标保留 raw 值，确保可追溯。
    """
    # 原始值来源：FlowMonitor + KPI_SUMMARY + SELF_HEAL_TIME 事件
    thr_vals = [f.get("throughput_mbps", 0) for f in (flows or []) if f.get("throughput_mbps") is not None]
    delay_vals = [f.get("avg_delay_ms", 0) for f in (flows or []) if f.get("avg_delay_ms") is not None]
    loss_vals = [f.get("loss_pct", 0) for f in (flows or []) if f.get("loss_pct") is not None]

    raw_thr = (sum(thr_vals) / len(thr_vals)) if thr_vals else 0.0
    raw_delay = (sum(delay_vals) / len(delay_vals)) if delay_vals else 0.0
    raw_loss = (sum(loss_vals) / len(loss_vals)) if loss_vals else 0.0
    raw_overhead = float((kpi_summary or {}).get("signalingOverheadPct", 0.0))
    raw_self_heal = (sum(self_heal_times) / len(self_heal_times)) if self_heal_times else 0.0

    # 归一化：正向指标直接映射，反向指标取 1-x（并截断）
    # 阈值按论文实验可解释范围设置，保证不同场景可比：
    # throughput: 0~5Mbps；delay: 0~200ms；loss: 0~100%；overhead: 0~100%；self-heal: 0~30s
    throughput_norm = _clamp01(raw_thr / 5.0)                    # 正向
    delay_norm = _clamp01(1.0 - raw_delay / 200.0)               # 反向
    loss_norm = _clamp01(1.0 - raw_loss / 100.0)                 # 反向
    overhead_norm = _clamp01(1.0 - raw_overhead / 100.0)         # 反向
    self_heal_speed_norm = _clamp01(1.0 - raw_self_heal / 30.0)  # 正向(速度) = 1-时间占比

    capability_metrics = {
        "throughputNorm": round(throughput_norm, 4),
        "selfHealSpeedNorm": round(self_heal_speed_norm, 4),
        "delayNorm": round(delay_norm, 4),
        "lossNorm": round(loss_norm, 4),
        "signalingOverheadNorm": round(overhead_norm, 4),
        "raw": {
            "throughputMbps": round(raw_thr, 4),
            "selfHealTimeSec": round(raw_self_heal, 4),
            "delayMs": round(raw_delay, 4),
            "lossPct": round(raw_loss, 4),
            "signalingOverheadPct": round(raw_overhead, 4),
        }
    }
    # 成本口径（越小越好）：便于保留“时延/丢包率/开销”原始方向的图表
    cost_metrics = {
        "throughputCostNorm": round(_clamp01(1.0 - throughput_norm), 4),  # 吞吐不足成本
        "selfHealTimeNorm": round(_clamp01(raw_self_heal / 30.0), 4),      # 自愈耗时占比
        "delayCostNorm": round(_clamp01(raw_delay / 200.0), 4),             # 时延成本
        "lossCostNorm": round(_clamp01(raw_loss / 100.0), 4),               # 丢包成本
        "signalingOverheadCostNorm": round(_clamp01(raw_overhead / 100.0), 4),  # 开销成本
        "raw": capability_metrics["raw"],
    }
    return capability_metrics, cost_metrics


def load_jsonl_log(path):
    """从 nms-state.jsonl 等 JSONL 文件加载记录，返回 build_node_states_from_jsonl 所需格式。"""
    records = []
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    r = json.loads(line)
                except json.JSONDecodeError:
                    continue
                # NS-3 输出: timestamp, node_id, join_state, pos_x/y/z, ip, energy, link_quality, role(可选)
                rec = {
                    "t": float(r.get("timestamp", 0)),
                    "nodeId": int(r.get("node_id", 0)),
                    "ip": r.get("ip", ""),
                    "energy": float(r.get("energy", 1.0)),
                    "link_quality": float(r.get("link_quality", 1.0)),
                    "speed": r.get("speed"),
                    "pos_x": float(r.get("pos_x", 0)),
                    "pos_y": float(r.get("pos_y", 0)),
                }
                if "join_state" in r:
                    rec["join_state"] = str(r.get("join_state", ""))
                if "role" in r and r.get("role"):
                    rec["role"] = str(r.get("role", ""))
                records.append(rec)
    except OSError:
        pass
    return records


def build_node_states_from_jsonl(records, time_bucket=0.5):
    """将 JSONL 记录按时间桶聚合为 nodeStatesByTime，供前端按时刻查询；含 pos 供位置插值。
    对 ip/role 做按节点前向填充，避免故障节点最后一条空 ip 污染同桶其它字段或误清空其它节点。"""
    records = sorted(records, key=lambda r: (r["t"], r.get("nodeId", 0)))
    by_t = {}
    last_ip = {}
    last_role = {}
    for r in records:
        t_b = round(r["t"] / time_bucket) * time_bucket
        if t_b not in by_t:
            by_t[t_b] = {}
        nid = r["nodeId"]
        ip = (r.get("ip") or "").strip()
        if not ip:
            ip = last_ip.get(nid, "")
        else:
            last_ip[nid] = ip
        role = (r.get("role") or "").strip()
        if not role:
            role = last_role.get(nid, "")
        else:
            last_role[nid] = role
        entry = {
            "ip": ip,
            "energy": r["energy"] if r["energy"] >= 0 else None,
            "link_quality": r["link_quality"] if r["link_quality"] >= 0 else None,
            "speed": r.get("speed"),
        }
        if "pos_x" in r or "pos_y" in r:
            entry["x"] = r.get("pos_x")
            entry["y"] = r.get("pos_y")
        if role:
            entry["role"] = role
        js = (r.get("join_state") or "").strip()
        if js:
            entry["join_state"] = js
        by_t[t_b][nid] = entry
    return by_t


def align_join_state_with_join_config(node_states_by_time, join_time_map):
    """快照时间已晚于 join_config 的 join_time 时，强制 join_state=joined，避免 JSONL 在接口刚 Up 前误标 pending/offline。"""
    if not node_states_by_time or not join_time_map:
        return
    for t_str, bucket in node_states_by_time.items():
        try:
            t_b = float(t_str)
        except (TypeError, ValueError):
            continue
        for nid_str, st in list(bucket.items()):
            try:
                nid = int(nid_str)
            except (TypeError, ValueError):
                continue
            jt = join_time_map.get(nid)
            if jt is None:
                continue
            if t_b + 1e-9 >= float(jt):
                if not isinstance(st, dict):
                    st = {}
                st["join_state"] = "joined"
                bucket[nid_str] = st


def merge_jsonl_best_ip_role_into_topology(nodes, jsonl_records):
    """每条节点取最后一次非空 ip/role，避免仅用文件末行且空 ip 导致拓扑丢地址。"""
    best_ip = {}
    best_role = {}
    best_join_state = {}
    for r in jsonl_records:
        nid = r["nodeId"]
        ip = (r.get("ip") or "").strip()
        if ip:
            best_ip[nid] = ip
        ro = (r.get("role") or "").strip()
        if ro:
            best_role[nid] = ro
        js = (r.get("join_state") or "").strip()
        if js:
            best_join_state[nid] = js
    for n in nodes:
        nid = n["id"]
        if nid in best_ip:
            n["ip"] = best_ip[nid]
        if nid in best_role:
            n["role"] = best_role[nid]
        if nid in best_join_state:
            n["joinState"] = best_join_state[nid]


def ensure_mesh_links(nodes, links, role_by_node, subnet_by_node, join_time):
    """Adhoc/TSN 子网内补充网状链路（两两相连），去重，并设置 link.t 与可选 link_quality。"""
    node_ids_by_subnet = {}
    for n in nodes:
        s = (n.get("subnet") or "other").lower()
        if s in ("adhoc", "tsn"):
            node_ids_by_subnet.setdefault(s, []).append(n["id"])
    seen = {(min(l["from"], l["to"]), max(l["from"], l["to"])) for l in links}
    for s, nids in node_ids_by_subnet.items():
        if len(nids) < 2:
            continue
        for i in range(len(nids)):
            for j in range(i + 1, len(nids)):
                a, b = nids[i], nids[j]
                key = (min(a, b), max(a, b))
                if key in seen:
                    continue
                seen.add(key)
                link_t = max(join_time.get(a, 0), join_time.get(b, 0))
                links.append({"from": key[0], "to": key[1], "type": "data", "t": link_t})
    return links


def run(dir_path, out_dir, join_config_path=None):
    dir_path = os.path.abspath(dir_path)
    if not os.path.isdir(dir_path):
        print("目录不存在:", dir_path)
        return
    out_dir = os.path.abspath(out_dir or dir_path)
    os.makedirs(out_dir, exist_ok=True)

    log_dir = os.path.join(dir_path, "log")
    vis_dir = os.path.join(dir_path, "visualization")
    perf_dir = os.path.join(dir_path, "performance")

    events = []
    if os.path.isdir(log_dir):
        for name in os.listdir(log_dir):
            if "nms-framework-log" in name and name.endswith(".txt"):
                events = load_events_from_log(os.path.join(log_dir, name))
                break
        if not events:
            for name in os.listdir(log_dir):
                if name.endswith(".txt"):
                    events = load_events_from_log(os.path.join(log_dir, name))
                    if events:
                        break
    jsonl_records = []
    if os.path.isdir(log_dir):
        for name in os.listdir(log_dir):
            if name.endswith(".jsonl"):
                jsonl_records = load_jsonl_log(os.path.join(log_dir, name))
                break
    node_states_by_time = {}
    if jsonl_records:
        node_states_by_time = build_node_states_from_jsonl(jsonl_records)
        # 转为可 JSON 序列化的格式（键为字符串）
        node_states_by_time = {str(k): {str(nid): v for nid, v in v.items()} for k, v in sorted(node_states_by_time.items())}
    if not events:
        print("未找到日志，将生成空事件列表")

    role_by_node, subnet_by_node = infer_node_role_from_events(events)
    join_time = get_join_time_by_node(events)
    if jsonl_records:
        join_time_jsonl = get_join_time_from_jsonl(jsonl_records)
        for nid, t in join_time_jsonl.items():
            if nid not in join_time:
                join_time[nid] = t
    # 从 join_config.json 补全入网时间并建立簇内链路（配置驱动拓扑）
    join_config_paths = []
    if join_config_path and os.path.isfile(join_config_path):
        join_config_paths.append(os.path.abspath(join_config_path))
    join_config_paths.extend([
        os.path.join(dir_path, "join_config.json"),
        os.path.join(dir_path, "..", "docs", "heterogeneous-nms", "join_config.json"),
        os.path.join(dir_path, "..", "..", "docs", "heterogeneous-nms", "join_config.json"),
    ])
    join_config = []
    for jpath in join_config_paths:
        jpath = os.path.normpath(jpath)
        join_config = load_join_config(jpath)
        if join_config:
            # 以 join_config 为权威：凡在 join_config 中的节点，入网时间一律用 JSON 的 join_time（保证 13/14/15 等按时序出现）
            for n in join_config:
                nid, jt = n["node_id"], n["join_time"]
                join_time[nid] = jt
            break
    align_join_state_with_join_config(node_states_by_time, join_time)
    # 用 join_config 补全 subnet，且当事件未推断出 Adhoc/DataLink SPN 时指定默认 SPN，保证自组网/数据链在拓扑中始终显示一个 SPN
    if join_config:
        for n in join_config:
            nid = n.get("node_id")
            sub = (n.get("subnet") or "").strip().lower()
            if sub and nid is not None and nid not in subnet_by_node:
                subnet_by_node[nid] = sub
        for sub in ("adhoc", "datalink"):
            has_spn = any(
                role_is_spn_for_backhaul(role_by_node.get(n.get("node_id")))
                for n in join_config
                if (n.get("subnet") or "").strip().lower() == sub
            )
            if not has_spn:
                nids = [n["node_id"] for n in join_config if (n.get("subnet") or "").strip().lower() == sub]
                if nids:
                    role_by_node[min(nids)] = "PRIMARY_SPN"
    offline_by_node = get_offline_time_by_node(events)
    power_off_by_node = get_power_off_time_from_jsonl(jsonl_records) if jsonl_records else {}

    node_pos = {}
    node_positions = {}
    links_raw = []
    if os.path.isdir(vis_dir):
        for name in os.listdir(vis_dir):
            if "heterogeneous" in name and name.endswith(".xml"):
                node_pos, node_positions, links_raw = load_topology_from_netanim_xml(
                    os.path.join(vis_dir, name)
                )
                break

    # 若有 join_config，已按 JSON 设置 join_time，不再做错峰；仅当无 join_config 且所有节点 joinTime 相同时才错峰
    if not join_config:
        all_nids = set(node_pos.keys()) if node_pos else set(ev["nodeId"] for ev in events)
        if len(all_nids) > 1:
            join_vals = {join_time.get(n, 0) for n in all_nids}
            if len(join_vals) <= 1:
                step = 0.5
                for i, nid in enumerate(sorted(all_nids)):
                    join_time[nid] = i * step

    # 基于配置与日志构建链路：有 join_config 时仅用 JSON 的 neighbors（intra_links），并只从 XML 保留回程链路，避免全联通
    intra_links = build_intra_cluster_links_from_join_config(join_config, join_time)
    if join_config:
        # 有 join_config 时不再使用 XML 的 data 链路，只保留回程（GMC-SPN），拓扑严格按 JSON neighbors
        backhaul_only = [l for l in links_raw if l.get("type") == "backhaul" or (l.get("from") == 0 or l.get("to") == 0)]
        links_raw_merged = intra_links + backhaul_only
    else:
        links_raw_merged = intra_links + links_raw
    links = dedup_links(links_raw_merged, role_by_node, join_time)

    # 节点列表：有 join_config 时以 JSON 为准（保证 16 节点、蜂窝 6 个等），否则从 node_pos/events
    nodes = []

    def random_xy(low=10, high=90):
        """为防止重叠，对无 init_pos 或不在 join_config 中的节点赋予随机坐标（不受 NetAnim 干扰）。"""
        return random.uniform(low, high), random.uniform(low, high)

    if join_config:
        # 强覆盖策略：完全以 join_config 的 init_pos 为准，无视 NetAnim；若无 init_pos 则用随机坐标避免重叠
        for n in join_config:
            nid = n["node_id"]
            if n.get("init_pos") and len(n["init_pos"]) >= 2:
                x, y = float(n["init_pos"][0]), float(n["init_pos"][1])
            else:
                x, y = random_xy()
            role = role_by_node.get(nid, "gmc" if n.get("subnet", "").strip().lower() == "gmc" else "node")
            sub = (n.get("subnet") or "").strip().lower()
            if not sub:
                sub = "other"
            # LTE 子网按 type 区分 spn(eNB) / ue(UE)
            if sub == "lte" and role == "node":
                t = (n.get("type") or "").strip().lower()
                if t == "enb":
                    role = "spn"
                elif t == "ue":
                    role = "ue"
            if role == "gmc":
                label = "GMC"
            elif role_is_spn_for_backhaul(role):
                label = f"SPN-{nid}"
            else:
                label = f"Node-{nid}"
            node_obj = {
                "id": nid, "label": label, "role": role, "subnet": sub,
                "x": x, "y": y,
                "joinTime": n.get("join_time", 0),
            }
            if n.get("ip"):
                node_obj["ip"] = n["ip"]
            if n.get("speed") is not None:
                try:
                    node_obj["speed"] = float(n["speed"])
                except (TypeError, ValueError):
                    pass
            off_t = min(offline_by_node.get(nid, 1e9), power_off_by_node.get(nid, 1e9))
            if off_t < 1e9:
                node_obj["offlineTime"] = off_t
            # 有 join_config 时一律不写 positions，前端仅用 x/y 固定绘图，避免 GMC 等节点跳跃
            if not join_config and nid in node_positions and node_positions.get(nid):
                node_obj["positions"] = [{"t": t, "x": px, "y": py} for t, px, py in node_positions[nid]]
            if join_config:
                node_obj.pop("positions", None)
            nodes.append(node_obj)
    elif node_pos:
        # 无 join_config 时节点不在 json 中，完全不以 NetAnim 为准，统一随机坐标防止重叠
        for nid in sorted(node_pos.keys()):
            x, y = random_xy()
            role = role_by_node.get(nid, "node")
            subnet = subnet_by_node.get(nid, "other")
            label = "GMC" if role == "gmc" else (f"SPN-{nid}" if role_is_spn_for_backhaul(role) else f"Node-{nid}")
            node_obj = {
                "id": nid, "label": label, "role": role, "subnet": subnet,
                "x": x, "y": y,
                "joinTime": join_time.get(nid, 0),
            }
            off_t = min(offline_by_node.get(nid, 1e9), power_off_by_node.get(nid, 1e9))
            if off_t < 1e9:
                node_obj["offlineTime"] = off_t
            if nid in node_positions and node_positions[nid]:
                node_obj["positions"] = [{"t": t, "x": px, "y": py} for t, px, py in node_positions[nid]]
            nodes.append(node_obj)
    else:
        # 仅从事件解析出的节点（无 join_config、无 node_pos），用随机坐标避免重叠
        seen = set()
        for ev in events:
            n = ev["nodeId"]
            if n in seen:
                continue
            seen.add(n)
            role = role_by_node.get(n, "node")
            subnet = subnet_by_node.get(n, "other")
            label = "GMC" if role == "gmc" else (f"SPN-{n}" if role_is_spn_for_backhaul(role) else f"Node-{n}")
            x, y = random_xy()
            node_obj = {
                "id": n, "label": label, "role": role, "subnet": subnet,
                "x": x, "y": y,
                "joinTime": join_time.get(n, 0),
            }
            off_t = min(offline_by_node.get(n, 1e9), power_off_by_node.get(n, 1e9))
            if off_t < 1e9:
                node_obj["offlineTime"] = off_t
            nodes.append(node_obj)

    # 不再调用 ensure_mesh_links：完全信任 build_intra_cluster_links_from_join_config + dedup_links 的链路，不强制 Mesh
    # links = ensure_mesh_links(...) 已移除，避免覆盖 JSON 拓扑

    if jsonl_records:
        merge_jsonl_best_ip_role_into_topology(nodes, jsonl_records)
        join_cfg_by_id = {n["node_id"]: n for n in join_config} if join_config else {}
        for n in nodes:
            jc = join_cfg_by_id.get(n["id"])
            if jc and jc.get("ip") and not (n.get("ip") or "").strip():
                n["ip"] = jc["ip"]
        last_by_node = {}
        positions_by_node = {}  # nid -> [(t, x, y), ...] 供自研可视化节点移动
        for r in jsonl_records:
            nid = r["nodeId"]
            last_by_node[nid] = r
            if "pos_x" in r or "pos_y" in r:
                if nid not in positions_by_node:
                    positions_by_node[nid] = []
                positions_by_node[nid].append((r["t"], r.get("pos_x", 0), r.get("pos_y", 0)))
        for nid in positions_by_node:
            positions_by_node[nid].sort(key=lambda v: v[0])
        for n in nodes:
            if n["id"] in last_by_node:
                r = last_by_node[n["id"]]
                if r.get("energy", -1) >= 0:
                    n["energy"] = r["energy"]
                if r.get("link_quality", -1) >= 0:
                    n["link_quality"] = r["link_quality"]
            # 有 join_config 时不写入 JSONL 轨迹，拓扑仅用 init_pos 导出的 x/y，避免前端按时间插值导致跳跃
            if not join_config and n["id"] in positions_by_node and positions_by_node[n["id"]]:
                n["positions"] = [{"t": t, "x": x, "y": y} for t, x, y in positions_by_node[n["id"]]]

    ch_hist = extract_adhoc_ch_elect_history(events)
    last_ch = ch_hist[-1]["ch"] if ch_hist else None
    topology = {
        "nodes": nodes,
        "links": links,
        "adhocCluster": {
            "clusterHead": last_ch,
            "chElectHistory": ch_hist,
        },
    }
    topo_path = os.path.join(out_dir, "topology.json")
    with open(topo_path, "w", encoding="utf-8") as f:
        json.dump(topology, f, ensure_ascii=False, indent=2)
    print("已写入:", topo_path)

    events_path = os.path.join(out_dir, "events.json")
    with open(events_path, "w", encoding="utf-8") as f:
        json.dump(events, f, ensure_ascii=False, indent=2)
    print("已写入:", events_path)

    flowmon_xml = None
    kpi_summary_json = None
    state_driven_kpi_json = None
    if os.path.isdir(perf_dir):
        for name in os.listdir(perf_dir):
            if "flowmon_stats" in name and name.endswith(".xml"):
                flowmon_xml = os.path.join(perf_dir, name)
            if "kpi-summary" in name and name.endswith(".json"):
                kpi_summary_json = os.path.join(perf_dir, name)
            if "state-driven-kpi-summary" in name and name.endswith(".json"):
                state_driven_kpi_json = os.path.join(perf_dir, name)
    stats = {"flows": [], "timeSeries": {}, "spnElectionCount": 0, "spnElectionBySubnet": {}}
    if flowmon_xml:
        stats["flows"] = load_flowmon_stats(flowmon_xml)
    stats_ts = extract_time_series_from_events(events)
    stats["timeSeries"] = {
        "energy": stats_ts["energy"],
        "linkQuality": stats_ts["linkQuality"],
        "mobility": stats_ts["mobility"],
        "score": stats_ts["score"],
    }
    stats["spnElectionCount"] = stats_ts["spnElectionCount"]
    stats["spnElectionBySubnet"] = stats_ts["spnElectionBySubnet"]
    if node_states_by_time:
        stats["nodeStatesByTime"] = node_states_by_time

    # --- 业务流解析与 stats.json 根节点三字段（供前端 Echarts/表格） ---
    business_flows = extract_business_flows_from_events(events)
    # 无 FLOW_START/FLOW_PERF 时用 flowmon 统计生成占位数据，保证业务图表有数据可展示
    if not business_flows and stats.get("flows"):
        for i, f in enumerate(stats["flows"], start=1):
            business_flows.append({
                "id": f.get("flowId", i),
                "type": "data",
                "priority": 1,
                "src": 0, "dst": 0,
                "path": [], "startTime": 0, "endTime": 100,
                "label": "Flow-" + str(f.get("flowId", i)),
                "delayMs": f.get("avg_delay_ms"),
                "throughputMbps": f.get("throughput_mbps"),
                "lossRate": (f.get("loss_pct", 0) / 100.0) if f.get("loss_pct") is not None else None,
            })
    business_features = build_business_features(business_flows)
    flow_perf_windows = extract_flow_perf_window_series(events)
    flow_performance = build_flow_performance_timeseries(
        business_flows, flow_perf_windows=flow_perf_windows, window_sec=0.5
    )
    kpi_summary = {}
    if kpi_summary_json and os.path.isfile(kpi_summary_json):
        try:
            with open(kpi_summary_json, "r", encoding="utf-8") as f:
                kpi_summary = json.load(f)
        except Exception:
            kpi_summary = {}
    self_heal_times = []
    for e in events:
        if str(e.get("event", "")).upper() == "SELF_HEAL_TIME":
            m = re.search(r"([\d.]+)\s*s", str(e.get("details", "")), re.I)
            if m:
                self_heal_times.append(parse_float(m.group(1), 0))
    network_radar_metrics, network_radar_costs = build_network_radar_metrics(
        stats.get("flows", []), kpi_summary, self_heal_times
    )
    state_driven_kpi = {}
    if state_driven_kpi_json and os.path.isfile(state_driven_kpi_json):
        try:
            with open(state_driven_kpi_json, "r", encoding="utf-8") as f:
                state_driven_kpi = json.load(f)
        except Exception:
            state_driven_kpi = {}

    stats["businessFlows"] = business_flows
    stats["businessFeatures"] = business_features
    stats["flowPerformance"] = flow_performance
    stats["networkRadarMetrics"] = network_radar_metrics
    stats["networkRadarCosts"] = network_radar_costs
    stats["stateDrivenKpi"] = state_driven_kpi

    stats_path = os.path.join(out_dir, "stats.json")
    with open(stats_path, "w", encoding="utf-8") as f:
        json.dump(stats, f, ensure_ascii=False, indent=2)
    print("已写入:", stats_path)

    if flowmon_xml:
        meta_path = os.path.join(out_dir, "meta.json")
        with open(meta_path, "w", encoding="utf-8") as f:
            json.dump({
                "flowmonXml": flowmon_xml,
                "stateDrivenKpiJson": state_driven_kpi_json,
                "eventsCount": len(events),
                "adhocChElectCount": len(ch_hist),
                "adhocClusterHead": last_ch,
            }, f, indent=2)
        print("已写入:", meta_path)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="导出可视化数据：topology.json + events.json")
    ap.add_argument("dir", nargs="?", default=".", help="仿真结果目录（如 simulation_results/时间戳）")
    ap.add_argument("-o", "--output", default=None, help="输出目录，默认与 dir 相同")
    ap.add_argument("--join-config", default=None, help="join_config.json 路径（可选，用于簇内拓扑）")
    args = ap.parse_args()
    run(args.dir, args.output, join_config_path=args.join_config)
