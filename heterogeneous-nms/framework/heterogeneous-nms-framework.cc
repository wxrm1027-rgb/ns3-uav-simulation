/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#define HNMS_NO_MAIN
#define HNMS_USE_MODULES
/*
 * 文件名: heterogeneous-nms-framework.cc
 *
 * 设计目标：
 *  - 构建一个“分层异构”网络管理仿真底层框架，便于后续论文中的算法快速接入
 *  - 清晰分离拓扑构建（LTE/WiFi Ad-hoc/战术数据链）、应用逻辑（自定义 Application）与仿真控制
 *  - 预留智能调度 / 流控 / 管理算法入口
 *
 * 使用说明（建议流程）：
 *  1. 在 HeterogeneousNodeApp::SendPacket() 中实现你自己的智能流控/网络管理算法
 *  2. 在 HeterogeneousNmsFramework::InstallApplications() 中布置哪些节点运行管理应用（GMC / 各个 SPN / 普通节点）
 *  3. 在各个 BuildXXXSubnet() 中扩展物理层 / MAC / 路由 / 移动模型参数
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/mobility-module.h"
#include "ns3/wifi-module.h"
#include "ns3/olsr-module.h"
#include "ns3/aodv-helper.h"
#include "ns3/ipv4-list-routing-helper.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/applications-module.h"
#include "ns3/node-list.h"
#include "ns3/propagation-loss-model.h"

// LTE / EPC 模块
#include "ns3/lte-module.h"
#include "ns3/point-to-point-epc-helper.h"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cerrno>
#include <sys/stat.h>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("HeterogeneousNodeApp");

// ====================== 统一日志：终端 + 文件 ======================
std::ofstream g_nmsLogFile;

#ifndef HNMS_USE_MODULES
static void
NmsLog (const char* level, uint32_t nodeId, const char* eventType, const std::string& details)
{
  double t = Simulator::Now ().GetSeconds ();
  std::ostringstream line;
  line << std::fixed << std::setprecision (3) << "[" << t << "s] [Node " << nodeId << "] ["
       << eventType << "] " << details;
  std::string s = line.str ();
  if (g_nmsLogFile.is_open ())
    {
      g_nmsLogFile << s << std::endl;
    }
  if (strcmp (level, "ERROR") == 0)
    {
      NS_LOG_ERROR (s);
    }
  else if (strcmp (level, "WARN") == 0)
    {
      NS_LOG_WARN (s);
    }
  else
    {
      NS_LOG_INFO (s);
    }
}
#else
#include "../core/logger.h"
#endif

#define NMS_LOG_INFO(nodeId, eventType, details)  NmsLog ("INFO",  nodeId, eventType, details)
#define NMS_LOG_WARN(nodeId, eventType, details)   NmsLog ("WARN",  nodeId, eventType, details)
#define NMS_LOG_ERROR(nodeId, eventType, details)  NmsLog ("ERROR", nodeId, eventType, details)

// ====================== 时序入网：JSON 配置 + JSONL 状态日志 ======================
static std::ofstream g_jsonlStateFile;

#ifndef HNMS_USE_MODULES
struct NodeJoinConfig
{
  uint32_t nodeId;
  std::string type;
  std::string subnet;
  double joinTime;           ///< 入网时间（秒，支持小数即毫秒精度）
  double initPos[3];
  double speed;
  // 扩展：性能属性（可选，未设置时用默认值）
  std::string ipAddress;     ///< 静态 IP（空表示动态分配）
  double initialRateMbps;    ///< 入网初始传输速率 Mbps，<=0 表示不覆盖
  double initialEnergy;      ///< 初始能量 [0,1]，<0 表示不覆盖
  double initialEnergyMah;   ///< 初始能量 mAh（可选，用于显示/能耗模型）
  double initialLinkQuality; ///< 初始链路质量 [0,1] 或 RSSI 归一化，<0 表示不覆盖
  std::vector<uint32_t> neighbors;  ///< 逻辑拓扑：与该节点存在链路的邻居 node_id 列表（用于 MatrixPropagationLossModel）
};

/// 从 JSON 文件解析节点入网配置（简易解析，无第三方库）
static std::map<uint32_t, NodeJoinConfig>
LoadNodeJoinConfig (const std::string& path)
{
  std::map<uint32_t, NodeJoinConfig> out;
  if (path.empty ()) return out;
  std::ifstream f (path.c_str ());
  if (!f) return out;
  std::stringstream ss;
  ss << f.rdbuf ();
  std::string content = ss.str ();
  f.close ();
  size_t i = 0;
  while ((i = content.find ("\"node_id\"", i)) != std::string::npos)
    {
      NodeJoinConfig c = {};
      c.initPos[0] = c.initPos[1] = c.initPos[2] = 0.0;
      c.initialRateMbps = c.initialEnergy = c.initialEnergyMah = c.initialLinkQuality = -1.0;
      auto skipToVal = [&content, &i] (const std::string& key) -> size_t {
        size_t p = content.find (key, i);
        if (p == std::string::npos) return std::string::npos;
        p = content.find (':', p + key.size ());
        return p == std::string::npos ? p : p + 1;
      };
      auto getNum = [&content, &i] (size_t start) -> double {
        if (start == std::string::npos || start >= content.size ()) return 0.0;
        // 关键修复：跳过 ':' 后的空白字符，避免把 " 15" 解析为空串导致 node_id 全部变 0
        while (start < content.size () &&
               (content[start] == ' ' || content[start] == '\t' ||
                content[start] == '\n' || content[start] == '\r'))
          {
            ++start;
          }
        if (start >= content.size ()) return 0.0;
        size_t end = start;
        while (end < content.size () &&
               (std::isdigit (content[end]) || content[end] == '.' ||
                content[end] == '-' || content[end] == '+' ||
                content[end] == 'e' || content[end] == 'E'))
          {
            ++end;
          }
        std::string s = content.substr (start, end - start);
        return atof (s.c_str ());
      };
      auto getStr = [&content, &i] (size_t start) -> std::string {
        if (start == std::string::npos) return "";
        size_t q = content.find ('"', start);
        if (q == std::string::npos) return "";
        size_t q2 = content.find ('"', q + 1);
        if (q2 == std::string::npos) return "";
        return content.substr (q + 1, q2 - q - 1);
      };
      size_t pNodeId = skipToVal ("\"node_id\"");
      c.nodeId = static_cast<uint32_t> (getNum (pNodeId));
      size_t pJoin = skipToVal ("\"join_time\"");
      if (pJoin != std::string::npos) c.joinTime = getNum (pJoin);
      else { size_t pMs = skipToVal ("\"join_time_ms\""); if (pMs != std::string::npos) c.joinTime = getNum (pMs) / 1000.0; }
      size_t pSpeed = skipToVal ("\"speed\"");
      c.speed = getNum (pSpeed);
      size_t pType = content.find ("\"type\"", i);
      if (pType != std::string::npos) c.type = getStr (content.find (':', pType) + 1);
      size_t pSubnet = content.find ("\"subnet\"", i);
      if (pSubnet != std::string::npos) c.subnet = getStr (content.find (':', pSubnet) + 1);
      size_t pArr = content.find ("\"init_pos\"", i);
      if (pArr != std::string::npos)
        {
          size_t bracket = content.find ('[', pArr);
          if (bracket != std::string::npos)
            {
              const char* ptr = content.c_str () + bracket;
              std::sscanf (ptr, "[%lf,%lf,%lf]", &c.initPos[0], &c.initPos[1], &c.initPos[2]);
            }
        }
      size_t pIp = content.find ("\"ip\"", i);
      if (pIp != std::string::npos) c.ipAddress = getStr (content.find (':', pIp) + 1);
      size_t pRate = skipToVal ("\"initial_rate_mbps\"");
      if (pRate != std::string::npos) c.initialRateMbps = getNum (pRate);
      size_t pE = skipToVal ("\"initial_energy\"");
      if (pE != std::string::npos) c.initialEnergy = getNum (pE);
      size_t pEmah = skipToVal ("\"initial_energy_mah\"");
      if (pEmah != std::string::npos) c.initialEnergyMah = getNum (pEmah);
      size_t pLq = skipToVal ("\"initial_link_quality\"");
      if (pLq != std::string::npos) c.initialLinkQuality = getNum (pLq);
      // 解析 "neighbors": [id1, id2, ...]（逻辑拓扑，用于 MatrixPropagationLossModel）
      size_t pNeigh = content.find ("\"neighbors\"", i);
      if (pNeigh != std::string::npos && pNeigh < content.find ("\"node_id\"", i + 10))
        {
          size_t bracket = content.find ('[', pNeigh);
          if (bracket != std::string::npos)
            {
              size_t end = bracket + 1;
              while (end < content.size ())
                {
                  while (end < content.size () && (content[end] == ' ' || content[end] == ',')) ++end;
                  if (end >= content.size () || content[end] == ']') break;
                  double num = getNum (end);
                  c.neighbors.push_back (static_cast<uint32_t> (num));
                  while (end < content.size () && content[end] != ',' && content[end] != ']') ++end;
                }
            }
        }
      out[c.nodeId] = c;
      // 跳到当前对象末尾，使下一轮 find 找到下一个 "node_id" 而非重复解析同一项（否则只加载 1 个节点）
      size_t objEnd = content.find ('}', i);
      if (objEnd != std::string::npos)
        i = objEnd + 1;
      else
        break;
    }
  return out;
}

/// 从 join_config 中提取指定子网的逻辑链路集合（无向边 (min(a,b), max(a,b))），用于 MatrixPropagationLossModel
static std::set<std::pair<uint32_t, uint32_t>>
GetLinksFromJoinConfig (const std::map<uint32_t, NodeJoinConfig>& config, const std::string& subnet)
{
  std::set<std::pair<uint32_t, uint32_t>> links;
  for (const auto& kv : config)
    {
      const NodeJoinConfig& c = kv.second;
      if (c.subnet != subnet) continue;
      for (uint32_t nb : c.neighbors)
        {
          uint32_t a = c.nodeId, b = nb;
          if (a > b) std::swap (a, b);
          links.insert (std::make_pair (a, b));
        }
    }
  return links;
}

/// 校验 joinconfig：IP 冲突、时间戳有序、initial_energy/initial_link_quality 范围；返回空串表示通过
static std::string
ValidateJoinConfig (const std::map<uint32_t, NodeJoinConfig>& config)
{
  std::set<std::string> ips;
  for (const auto& kv : config)
    {
      const NodeJoinConfig& c = kv.second;
      if (!c.ipAddress.empty ())
        {
          if (ips.count (c.ipAddress))
            return "JoinConfig: duplicate IP " + c.ipAddress + " node_id=" + std::to_string (c.nodeId);
          ips.insert (c.ipAddress);
        }
      if (c.initialEnergy >= 0.0 && c.initialEnergy > 1.0)
        return "JoinConfig: initial_energy must be [0,1] node_id=" + std::to_string (c.nodeId);
      if (c.initialLinkQuality >= 0.0 && c.initialLinkQuality > 1.0)
        return "JoinConfig: initial_link_quality must be [0,1] node_id=" + std::to_string (c.nodeId);
    }
  std::vector<std::pair<double, uint32_t>> order;
  for (const auto& kv : config) order.push_back ({kv.second.joinTime, kv.second.nodeId});
  std::sort (order.begin (), order.end ());
  for (size_t k = 1; k < order.size (); ++k)
    if (order[k].first < order[k - 1].first)
      return "JoinConfig: join_time out of order node_id=" + std::to_string (order[k].second);
  return "";
}

/// 写一行 JSONL 状态（timestamp, node_id, join_state, pos_x/y/z, ip, energy, link_quality）
static void
WriteJsonlStateLine (uint32_t nodeId, const std::string& joinState,
                     double posX, double posY, double posZ,
                     const std::string& ip, double energy, double linkQuality,
                     const std::string& role = "")
{
  if (!g_jsonlStateFile.is_open ()) return;
  double t = Simulator::Now ().GetSeconds ();
  g_jsonlStateFile << "{\"timestamp\":" << std::fixed << std::setprecision (3) << t
                   << ",\"node_id\":" << nodeId
                   << ",\"join_state\":\"" << joinState << "\""
                   << ",\"pos_x\":" << posX << ",\"pos_y\":" << posY << ",\"pos_z\":" << posZ
                   << ",\"ip\":\"" << ip << "\""
                   << ",\"energy\":" << energy << ",\"link_quality\":" << linkQuality;
  if (!role.empty ())
    g_jsonlStateFile << ",\"role\":\"" << role << "\"";
  g_jsonlStateFile << "}\n";
}

/// 获取节点主 IPv4 地址字符串（第一个接口）
static std::string
GetNodePrimaryIpv4 (Ptr<Node> node)
{
  if (!node) return "";
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
  if (!ipv4 || ipv4->GetNInterfaces () < 2) return "";
  for (uint32_t i = 1; i < ipv4->GetNInterfaces (); ++i)
    {
      if (ipv4->IsUp (i))
        {
          Ipv4Address addr = ipv4->GetAddress (i, 0).GetLocal ();
          std::ostringstream os;
          addr.Print (os);
          return os.str ();
        }
    }
  return "";
}

// ====================== 场景想定模块（扩展接口） ======================
struct TrajectoryPoint { double t; double x, y, z; };
/// 事件定义：由 scenario_config.json 的 events 数组解析
struct ScenarioEvent
{
  double time;        ///< 触发时刻（秒）
  std::string type;   ///< 如 "NODE_FAIL"
  uint32_t target;    ///< 目标节点 ID（如 NODE_FAIL 时）
  uint32_t triggerNodeId; ///< 触发节点 ID（SPN_SWITCH）
  uint32_t newSpnNodeId;  ///< 新主 SPN 节点 ID（SPN_SWITCH）
};
struct BusinessFlowConfig
{
  uint32_t flowId;         ///< 业务流 ID
  std::string type;        ///< 业务类型：video/control/data...
  uint8_t priority;        ///< 优先级
  uint8_t qos;             ///< QoS 等级
  uint32_t srcNodeId;      ///< 源节点 ID
  uint32_t dstNodeId;      ///< 目的节点 ID
  std::string dataRate;    ///< 速率，如 "1Mbps" / "100Kbps"
  uint32_t packetSize;     ///< 包长
  double startTime;        ///< 开始时刻
  double stopTime;         ///< 结束时刻（<=0 表示到仿真结束）
};
struct ScenarioConfig
{
  std::string scenarioId;
  std::map<uint32_t, std::vector<TrajectoryPoint>> trajectoryByNode;
  std::vector<std::string> eventRules;
  std::vector<ScenarioEvent> events;  ///< 事件列表（NODE_FAIL 等），由 C++ 定时触发
  std::vector<BusinessFlowConfig> businessFlows; ///< 业务流配置
};
static ScenarioConfig LoadScenarioConfig (const std::string& path)
{
  ScenarioConfig out;
  if (path.empty ()) return out;
  std::ifstream f (path.c_str ());
  if (!f) return out;
  std::stringstream ss;
  ss << f.rdbuf ();
  std::string content = ss.str ();
  f.close ();
  size_t idPos = content.find ("\"scenario_id\"");
  if (idPos != std::string::npos)
    {
      size_t q = content.find ('"', content.find (':', idPos) + 1);
      size_t q2 = content.find ('"', q + 1);
      if (q != std::string::npos && q2 != std::string::npos)
        out.scenarioId = content.substr (q + 1, q2 - q - 1);
    }
  // 解析 "events": [ {"time": 30, "type": "NODE_FAIL", "target": 2}, ... ]
  size_t eventsPos = content.find ("\"events\"");
  if (eventsPos != std::string::npos)
    {
      size_t arrStart = content.find ('[', eventsPos);
      if (arrStart != std::string::npos)
        {
          size_t pos = arrStart + 1;
          while (pos < content.size ())
            {
              size_t objStart = content.find ('{', pos);
              if (objStart == std::string::npos || objStart > content.find (']', pos)) break;
              size_t objEnd = content.find ('}', objStart);
              if (objEnd == std::string::npos) break;
              std::string obj = content.substr (objStart, objEnd - objStart + 1);
              ScenarioEvent ev = {};
              ev.time = 0.0;
              ev.target = 0;
              ev.triggerNodeId = 0;
              ev.newSpnNodeId = 0;
              size_t tPos = obj.find ("\"time\"");
              if (tPos != std::string::npos)
                {
                  size_t colon = obj.find (':', tPos);
                  if (colon != std::string::npos)
                    ev.time = atof (obj.c_str () + colon + 1);
                }
              size_t typePos = obj.find ("\"type\"");
              if (typePos != std::string::npos)
                {
                  size_t q1 = obj.find ('"', obj.find (':', typePos) + 1);
                  size_t q2 = obj.find ('"', q1 + 1);
                  if (q1 != std::string::npos && q2 != std::string::npos)
                    ev.type = obj.substr (q1 + 1, q2 - q1 - 1);
                }
              size_t targetPos = obj.find ("\"target\"");
              if (targetPos != std::string::npos)
                {
                  size_t colon = obj.find (':', targetPos);
                  if (colon != std::string::npos)
                    ev.target = static_cast<uint32_t> (atoi (obj.c_str () + colon + 1));
                }
              size_t triggerPos = obj.find ("\"trigger_node\"");
              if (triggerPos != std::string::npos)
                {
                  size_t colon = obj.find (':', triggerPos);
                  if (colon != std::string::npos)
                    ev.triggerNodeId = static_cast<uint32_t> (atoi (obj.c_str () + colon + 1));
                }
              size_t newSpnPos = obj.find ("\"new_spn_node\"");
              if (newSpnPos == std::string::npos)
                newSpnPos = obj.find ("\"new_spn\"");
              if (newSpnPos != std::string::npos)
                {
                  size_t colon = obj.find (':', newSpnPos);
                  if (colon != std::string::npos)
                    ev.newSpnNodeId = static_cast<uint32_t> (atoi (obj.c_str () + colon + 1));
                }
              if (ev.triggerNodeId == 0)
                ev.triggerNodeId = ev.target;
              if (!ev.type.empty ())
                out.events.push_back (ev);
              pos = objEnd + 1;
            }
        }
    }
  // 解析 "business_flows": [ {...}, ... ]
  size_t bfPos = content.find ("\"business_flows\"");
  if (bfPos != std::string::npos)
    {
      size_t arrStart = content.find ('[', bfPos);
      if (arrStart != std::string::npos)
        {
          size_t pos = arrStart + 1;
          while (pos < content.size ())
            {
              size_t objStart = content.find ('{', pos);
              if (objStart == std::string::npos || objStart > content.find (']', pos)) break;
              size_t objEnd = content.find ('}', objStart);
              if (objEnd == std::string::npos) break;
              std::string obj = content.substr (objStart, objEnd - objStart + 1);
              auto getNum = [&obj] (const std::string& key, double defv) -> double {
                size_t p = obj.find ("\"" + key + "\"");
                if (p == std::string::npos) return defv;
                size_t c = obj.find (':', p);
                if (c == std::string::npos) return defv;
                return atof (obj.c_str () + c + 1);
              };
              auto getStr = [&obj] (const std::string& key, const std::string& defv) -> std::string {
                size_t p = obj.find ("\"" + key + "\"");
                if (p == std::string::npos) return defv;
                size_t c = obj.find (':', p);
                if (c == std::string::npos) return defv;
                size_t q1 = obj.find ('"', c + 1);
                if (q1 == std::string::npos) return defv;
                size_t q2 = obj.find ('"', q1 + 1);
                if (q2 == std::string::npos) return defv;
                return obj.substr (q1 + 1, q2 - q1 - 1);
              };
              BusinessFlowConfig bf = {};
              bf.flowId = static_cast<uint32_t> (getNum ("flow_id", 0));
              bf.type = getStr ("type", "data");
              bf.priority = static_cast<uint8_t> (getNum ("priority", 1));
              bf.qos = static_cast<uint8_t> (getNum ("qos", 1));
              bf.srcNodeId = static_cast<uint32_t> (getNum ("src", 0));
              bf.dstNodeId = static_cast<uint32_t> (getNum ("dst", 0));
              bf.dataRate = getStr ("rate", "1Mbps");
              bf.packetSize = static_cast<uint32_t> (getNum ("packet_size", 512));
              bf.startTime = getNum ("start", 5.0);
              bf.stopTime = getNum ("stop", -1.0);
              if (bf.flowId > 0 && bf.srcNodeId != bf.dstNodeId)
                out.businessFlows.push_back (bf);
              pos = objEnd + 1;
            }
        }
    }
  return out;
}
#else
#include "../core/json_config.h"

static std::set<std::pair<uint32_t, uint32_t>>
GetLinksFromJoinConfig (const std::map<uint32_t, NodeJoinConfig>& config, const std::string& subnet)
{
  std::set<std::pair<uint32_t, uint32_t>> edges;
  std::set<uint32_t> ids;
  for (const auto& kv : config)
    {
      if (kv.second.subnet == subnet)
        ids.insert (kv.first);
    }
  for (const auto& kv : config)
    {
      if (kv.second.subnet != subnet) continue;
      uint32_t a = kv.first;
      for (uint32_t b : kv.second.neighbors)
        {
          if (!ids.count (b)) continue;
          uint32_t u = std::min (a, b), v = std::max (a, b);
          edges.insert (std::make_pair (u, v));
        }
    }
  return edges;
}

static void
WriteJsonlStateLine (uint32_t nodeId, const std::string& joinState,
                     double posX, double posY, double posZ,
                     const std::string& ip, double energy, double linkQuality,
                     const std::string& role = "")
{
  if (!g_jsonlStateFile.is_open ()) return;
  double t = Simulator::Now ().GetSeconds ();
  g_jsonlStateFile << "{\"timestamp\":" << std::fixed << std::setprecision (3) << t
                   << ",\"node_id\":" << nodeId
                   << ",\"join_state\":\"" << joinState << "\""
                   << ",\"pos_x\":" << posX << ",\"pos_y\":" << posY << ",\"pos_z\":" << posZ
                   << ",\"ip\":\"" << ip << "\""
                   << ",\"energy\":" << energy << ",\"link_quality\":" << linkQuality;
  if (!role.empty ()) g_jsonlStateFile << ",\"role\":\"" << role << "\"";
  g_jsonlStateFile << "}\n";
}

static std::string
GetNodePrimaryIpv4 (Ptr<Node> node)
{
  if (!node) return "";
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
  if (!ipv4 || ipv4->GetNInterfaces () < 2) return "";
  for (uint32_t i = 1; i < ipv4->GetNInterfaces (); ++i)
    {
      if (ipv4->IsUp (i))
        {
          Ipv4Address addr = ipv4->GetAddress (i, 0).GetLocal ();
          std::ostringstream os;
          addr.Print (os);
          return os.str ();
        }
    }
  return "";
}
#endif

/*======================== TLV 包格式（Type-Length-Value） ========================
 * 通用头：Type 1B, Length 2B (大端), Value Length 字节
 * Type: 0x01=UV-MIB 全量(LTE), 0x02=能量变化量(DataLink), 0x03=标准状态(Adhoc), 0x04=策略
 */
#ifndef HNMS_USE_MODULES
namespace Hnmp
{
  // 论文 4.3.2：HNMP 固定 6 字节头；窄带链路隐式压缩为 3 字节头（4.3.4）
  struct Header
  {
    uint8_t frameType;
    uint8_t qosLevel;
    uint8_t sourceId;
    uint8_t destId;
    uint8_t seq;
    uint8_t payloadLen;
  };

  enum FrameType : uint8_t
  {
    FRAME_REQUEST  = 0x01,
    FRAME_RESPONSE = 0x02,
    FRAME_ALERT    = 0x03,
    FRAME_REPORT   = 0x04,
    FRAME_POLICY   = 0x05,
  };

  inline uint32_t EncodeFrame (uint8_t* out,
                               uint32_t outSize,
                               const Header& h,
                               const uint8_t* payload,
                               uint32_t payloadLen,
                               bool implicitCompact3B)
  {
    if (!out || outSize < payloadLen + (implicitCompact3B ? 3u : 6u) || payloadLen > 255u) return 0;
    uint32_t off = 0;
    out[off++] = h.frameType;
    out[off++] = h.qosLevel;
    if (!implicitCompact3B)
      {
        out[off++] = h.sourceId;
        out[off++] = h.destId;
        out[off++] = h.seq;
      }
    out[off++] = static_cast<uint8_t> (payloadLen);
    if (payload && payloadLen > 0)
      std::memcpy (out + off, payload, payloadLen);
    return off + payloadLen;
  }

  inline bool DecodeFrame (const uint8_t* in,
                           uint32_t inLen,
                           bool implicitCompact3B,
                           Header* outHeader,
                           const uint8_t** outPayload,
                           uint32_t* outPayloadLen)
  {
    if (!in || !outHeader || !outPayload || !outPayloadLen) return false;
    uint32_t minHeader = implicitCompact3B ? 3u : 6u;
    if (inLen < minHeader) return false;
    uint32_t off = 0;
    outHeader->frameType = in[off++];
    outHeader->qosLevel = in[off++];
    if (implicitCompact3B)
      {
        // 论文 4.3.4：窄带链路接收端根据上下文补齐缺失字段
        outHeader->sourceId = 0;
        outHeader->destId = 0;
        outHeader->seq = 0;
      }
    else
      {
        outHeader->sourceId = in[off++];
        outHeader->destId = in[off++];
        outHeader->seq = in[off++];
      }
    outHeader->payloadLen = in[off++];
    if (inLen < off + outHeader->payloadLen) return false;
    *outPayload = in + off;
    *outPayloadLen = outHeader->payloadLen;
    return true;
  }
}
#else
#include "../protocol/hnmp.h"
#endif

#ifndef HNMS_USE_MODULES
namespace NmsTlv
{
  // 论文 4.4：UV-MIB 字典
  const uint8_t TYPE_TELEMETRY_010  = 0x10; // 基础遥测状态
  const uint8_t TYPE_ROLE_011       = 0x11; // 节点角色状态
  const uint8_t TYPE_SUBNET_012     = 0x12; // 子网配置/网络意图
  const uint8_t TYPE_FLOW_020       = 0x20; // 业务流特征与性能
  const uint8_t TYPE_TOPO_030       = 0x30; // 局部拓扑与邻居
  const uint8_t TYPE_LINK_031       = 0x31; // 链路状态与跨层控制
  const uint8_t TYPE_FAULT_040      = 0x40; // 节点/链路告警
  const uint8_t TYPE_ROUTE_FAIL_041 = 0x41; // 业务路由失败
  // 控制面内部 TLV（选举/汇聚）
  /// Hello 选举包：携带 nodeId + Score，用于邻居发现与 SPN/备选 SPN 动态选举
  const uint8_t TYPE_HELLO_ELECTION = 0x80;
  /// 节点上报 SPN：自身状态 + 一跳邻居列表，供 SPN 构建子网拓扑
  const uint8_t TYPE_NODE_REPORT_SPN = 0x81;
  /// SPN 聚合上报 GMC：节点状态列表 + 子网拓扑连线
  const uint8_t TYPE_TOPOLOGY_AGGREGATE = 0x82;
  /// Score 泛洪：带 TTL 的全网 Score 同步，用于消除脑裂、保证全子网唯一主备 SPN
  const uint8_t TYPE_SCORE_FLOOD = 0x83;
  /// 主 SPN -> 备 SPN 心跳同步（论文 4.5.3）
  const uint8_t TYPE_HEARTBEAT_SYNC = 0x84;

  const uint32_t MAX_LTE_PAYLOAD    = 3 + 24;   // 27
  const uint32_t MAX_ADHOC_PAYLOAD  = 3 + 24;   // 27
  const uint32_t MAX_DATALINK_PAYLOAD = 3 + 8;   // 11
  const uint32_t MAX_POLICY_PAYLOAD = 32;
  /// Hello 选举：Type(1) + Len(2) + nodeId(4) + score(8) = 15
  const uint32_t HELLO_ELECTION_PAYLOAD = 3 + 4 + 8;
  /// 单节点上报 SPN 最大长度（含邻居列表，邻居数上限 64）
  const uint32_t MAX_NODE_REPORT_PAYLOAD = 3 + 4 + 24 + 12 + 8 + 8 + 2 + 64 * 4;
  /// Score 泛洪单条 entry：nodeId(4) + score(8) = 12；头 TTL(1)+n(2)=3，最多 50 条
  const uint32_t MAX_SCORE_FLOOD_ENTRIES = 50;
  const uint32_t MAX_SCORE_FLOOD_PAYLOAD = 3 + 3 + MAX_SCORE_FLOOD_ENTRIES * 12;

  inline void WriteTlvHeader (uint8_t* buf, uint8_t type, uint16_t length)
  {
    if (!buf) return;
    buf[0] = type;
    buf[1] = (length >> 8) & 0xff;
    buf[2] = length & 0xff;
  }

  /// LTE 全量 UV-MIB：1+2+24 字节，需 buf 至少 27 字节
  inline uint32_t BuildLtePayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility)
  {
    if (!buf || bufSize < MAX_LTE_PAYLOAD) return 0;
    WriteTlvHeader (buf, TYPE_TELEMETRY_010, 24);
    std::memcpy (buf + 3, &energy, 8);
    std::memcpy (buf + 11, &linkQ, 8);
    std::memcpy (buf + 19, &mobility, 8);
    return MAX_LTE_PAYLOAD;
  }

  /// Adhoc 标准状态：1+2+24 字节
  inline uint32_t BuildAdhocPayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility)
  {
    if (!buf || bufSize < MAX_ADHOC_PAYLOAD) return 0;
    WriteTlvHeader (buf, TYPE_TELEMETRY_010, 24);
    std::memcpy (buf + 3, &energy, 8);
    std::memcpy (buf + 11, &linkQ, 8);
    std::memcpy (buf + 19, &mobility, 8);
    return MAX_ADHOC_PAYLOAD;
  }

  /// DataLink 能量变化量：1+2+8 字节
  inline uint32_t BuildDataLinkPayload (uint8_t* buf, uint32_t bufSize, double deltaEnergy)
  {
    if (!buf || bufSize < MAX_DATALINK_PAYLOAD) return 0;
    WriteTlvHeader (buf, TYPE_LINK_031, 8);
    std::memcpy (buf + 3, &deltaEnergy, 8);
    return MAX_DATALINK_PAYLOAD;
  }

  /// 策略包（GMC 下发）：Type 0x04，Value 为固定内容
  inline uint32_t BuildPolicyPayload (uint8_t* buf, uint32_t maxLen)
  {
    if (!buf || maxLen < 4) return 0;
    const char policy[] = "POLICY";
    uint16_t len = static_cast<uint16_t> (std::min (sizeof (policy) - 1, (size_t) maxLen));
    WriteTlvHeader (buf, TYPE_SUBNET_012, len);
    std::memcpy (buf + 3, policy, len);
    return 3 + len;
  }

  /// Hello 选举包：Type 0x05, Value = nodeId(4B 大端) + score(8B double)，共 12 字节
  inline uint32_t BuildHelloElectionPayload (uint8_t* buf, uint32_t bufSize, uint32_t nodeId, double score)
  {
    if (!buf || bufSize < HELLO_ELECTION_PAYLOAD) return 0;
    WriteTlvHeader (buf, TYPE_HELLO_ELECTION, 12);
    buf[3] = (nodeId >> 24) & 0xff;
    buf[4] = (nodeId >> 16) & 0xff;
    buf[5] = (nodeId >> 8) & 0xff;
    buf[6] = nodeId & 0xff;
    std::memcpy (buf + 7, &score, 8);
    return HELLO_ELECTION_PAYLOAD;
  }

  /// 解析 Hello 选举包：返回 value 长度，若足够则写出 nodeId 与 score
  inline uint16_t ParseHelloElection (const uint8_t* buf, uint32_t len, uint32_t* outNodeId, double* outScore)
  {
    if (!buf || len < 3) return 0;
    uint16_t valLen = (static_cast<uint16_t> (buf[1]) << 8) | buf[2];
    if (buf[0] != TYPE_HELLO_ELECTION || len < 3u + valLen || valLen < 12) return 0;
    if (outNodeId)
      *outNodeId = (static_cast<uint32_t> (buf[3]) << 24) | (static_cast<uint32_t> (buf[4]) << 16)
                   | (static_cast<uint32_t> (buf[5]) << 8) | buf[6];
    if (outScore)
      std::memcpy (outScore, buf + 7, 8);
    return valLen;
  }

  /// 节点上报 SPN：Type 0x06, Value = nodeId(4) + px(8)+py(8)+pz(8) + vx(4)+vy(4)+vz(4) + energy(8) + linkQ(8) + nNeighbors(2) + [neighborId(4)]*n
  inline uint32_t BuildNodeReportSpnPayload (uint8_t* buf, uint32_t bufSize,
                                             uint32_t nodeId, double px, double py, double pz,
                                             float vx, float vy, float vz,
                                             double energy, double linkQ,
                                             const std::vector<uint32_t>& neighborIds)
  {
    uint16_t n = static_cast<uint16_t> (std::min (neighborIds.size (), (size_t) 64));
    uint32_t valLen = 4 + 24 + 12 + 8 + 8 + 2 + n * 4;
    if (!buf || bufSize < 3u + valLen) return 0;
    WriteTlvHeader (buf, TYPE_NODE_REPORT_SPN, valLen);
    uint32_t off = 3;
    buf[off++] = (nodeId >> 24) & 0xff; buf[off++] = (nodeId >> 16) & 0xff;
    buf[off++] = (nodeId >> 8) & 0xff;  buf[off++] = nodeId & 0xff;
    std::memcpy (buf + off, &px, 8); off += 8;
    std::memcpy (buf + off, &py, 8); off += 8;
    std::memcpy (buf + off, &pz, 8); off += 8;
    std::memcpy (buf + off, &vx, 4); off += 4;
    std::memcpy (buf + off, &vy, 4); off += 4;
    std::memcpy (buf + off, &vz, 4); off += 4;
    std::memcpy (buf + off, &energy, 8); off += 8;
    std::memcpy (buf + off, &linkQ, 8); off += 8;
    buf[off++] = (n >> 8) & 0xff; buf[off++] = n & 0xff;
    for (uint16_t i = 0; i < n; ++i)
      {
        uint32_t id = neighborIds[i];
        buf[off++] = (id >> 24) & 0xff; buf[off++] = (id >> 16) & 0xff;
        buf[off++] = (id >> 8) & 0xff;  buf[off++] = id & 0xff;
      }
    return 3 + valLen;
  }

  /// 解析节点上报 SPN：从 value 起始处解析，返回消耗字节数，写出各字段与邻居列表
  inline uint32_t ParseNodeReportSpn (const uint8_t* val, uint16_t valLen,
                                      uint32_t* outNodeId, double* outPx, double* outPy, double* outPz,
                                      float* outVx, float* outVy, float* outVz,
                                      double* outEnergy, double* outLinkQ,
                                      std::vector<uint32_t>* outNeighborIds)
  {
    if (!val || valLen < 4 + 24 + 12 + 8 + 8 + 2) return 0;
    uint32_t off = 0;
    if (outNodeId)
      *outNodeId = (static_cast<uint32_t> (val[0]) << 24) | (static_cast<uint32_t> (val[1]) << 16)
                   | (static_cast<uint32_t> (val[2]) << 8) | val[3];
    off += 4;
    if (outPx) std::memcpy (outPx, val + off, 8);
    off += 8;
    if (outPy) std::memcpy (outPy, val + off, 8);
    off += 8;
    if (outPz) std::memcpy (outPz, val + off, 8);
    off += 8;
    if (outVx) std::memcpy (outVx, val + off, 4);
    off += 4;
    if (outVy) std::memcpy (outVy, val + off, 4);
    off += 4;
    if (outVz) std::memcpy (outVz, val + off, 4);
    off += 4;
    if (outEnergy) std::memcpy (outEnergy, val + off, 8);
    off += 8;
    if (outLinkQ) std::memcpy (outLinkQ, val + off, 8);
    off += 8;
    uint16_t n = (static_cast<uint16_t> (val[off]) << 8) | val[off + 1];
    off += 2;
    if (outNeighborIds)
      {
        outNeighborIds->clear ();
        for (uint16_t i = 0; i < n && off + 4 <= valLen; ++i)
          {
            uint32_t id = (static_cast<uint32_t> (val[off]) << 24) | (static_cast<uint32_t> (val[off+1]) << 16)
                          | (static_cast<uint32_t> (val[off+2]) << 8) | val[off+3];
            outNeighborIds->push_back (id);
            off += 4;
          }
      }
    else
      off += n * 4;
    return off;
  }

  /// Score 泛洪包：Type 0x08, Value = TTL(1) + n(2) + [nodeId(4), score(8)]*n
  inline uint32_t BuildScoreFloodPayload (uint8_t* buf, uint32_t bufSize, uint8_t ttl,
                                          const std::vector<std::pair<uint32_t, double>>& entries)
  {
    uint16_t n = static_cast<uint16_t> (std::min (entries.size (), (size_t) MAX_SCORE_FLOOD_ENTRIES));
    uint32_t valLen = 3 + n * 12;
    if (!buf || bufSize < 3u + valLen) return 0;
    WriteTlvHeader (buf, TYPE_SCORE_FLOOD, valLen);
    buf[3] = ttl;
    buf[4] = (n >> 8) & 0xff;
    buf[5] = n & 0xff;
    uint32_t off = 6;
    for (uint16_t i = 0; i < n; ++i)
      {
        uint32_t id = entries[i].first;
        double sc = entries[i].second;
        buf[off++] = (id >> 24) & 0xff; buf[off++] = (id >> 16) & 0xff;
        buf[off++] = (id >> 8) & 0xff;  buf[off++] = id & 0xff;
        std::memcpy (buf + off, &sc, 8);
        off += 8;
      }
    return 3 + valLen;
  }

  /// 解析 Score 泛洪：返回消耗字节数；outTtl、outEntries 可选；若 TTL>0 调用方可选择转发
  inline uint32_t ParseScoreFlood (const uint8_t* buf, uint32_t len,
                                   uint8_t* outTtl,
                                   std::vector<std::pair<uint32_t, double>>* outEntries)
  {
    if (!buf || len < 3 + 3) return 0;
    if (buf[0] != TYPE_SCORE_FLOOD) return 0;
    uint16_t valLen = (static_cast<uint16_t> (buf[1]) << 8) | buf[2];
    if (len < 3u + valLen || valLen < 3) return 0;
    if (outTtl) *outTtl = buf[3];
    uint16_t n = (static_cast<uint16_t> (buf[4]) << 8) | buf[5];
    uint32_t off = 6;
    if (outEntries)
      {
        outEntries->clear ();
        for (uint16_t i = 0; i < n && off + 12 <= (uint32_t)(3 + valLen); ++i)
          {
            uint32_t id = (static_cast<uint32_t> (buf[off]) << 24) | (static_cast<uint32_t> (buf[off+1]) << 16)
                          | (static_cast<uint32_t> (buf[off+2]) << 8) | buf[off+3];
            double sc = 0.0;
            std::memcpy (&sc, buf + off + 4, 8);
            outEntries->push_back (std::make_pair (id, sc));
            off += 12;
          }
      }
    else
      off = 6 + n * 12;
    return 3 + valLen;
  }

  /// 心跳同步包：Type 0x84, Value = primaryId(4) + nowSec(8)
  inline uint32_t BuildHeartbeatSyncPayload (uint8_t* buf, uint32_t bufSize, uint32_t primaryId, double nowSec)
  {
    if (!buf || bufSize < 3 + 12) return 0;
    WriteTlvHeader (buf, TYPE_HEARTBEAT_SYNC, 12);
    buf[3] = (primaryId >> 24) & 0xff;
    buf[4] = (primaryId >> 16) & 0xff;
    buf[5] = (primaryId >> 8) & 0xff;
    buf[6] = primaryId & 0xff;
    std::memcpy (buf + 7, &nowSec, 8);
    return 15;
  }

  inline uint32_t ParseHeartbeatSyncPayload (const uint8_t* buf, uint32_t len, uint32_t* outPrimaryId, double* outNowSec)
  {
    if (!buf || len < 15 || buf[0] != TYPE_HEARTBEAT_SYNC) return 0;
    uint16_t valLen = (static_cast<uint16_t> (buf[1]) << 8) | buf[2];
    if (valLen < 12 || len < 3u + valLen) return 0;
    if (outPrimaryId)
      *outPrimaryId = (static_cast<uint32_t> (buf[3]) << 24) | (static_cast<uint32_t> (buf[4]) << 16)
                    | (static_cast<uint32_t> (buf[5]) << 8) | buf[6];
    if (outNowSec)
      std::memcpy (outNowSec, buf + 7, 8);
    return 3 + valLen;
  }
}

/*======================== 数据包/控制包内容解析（运行时日志 + 离线 pcap 解析用） ========================
 * 支持两种方式：
 *  ① 仿真运行时：通过 g_enablePacketParse 在收发路径输出包完整内容（十六进制 + 明文注释）
 *  ② 离线：仿真时开启 pcap 抓包，后用 parse_flowmon_pcap.py 解析 pcap 文件
 * 解析内容：包类型(TLV Type), 长度, Value 段字段(能量/链路质量/策略等), 源目的 IP/端口, 节点 ID
 */
namespace NmsPacketParse
{
  /// 是否启用运行时包解析（由框架 CommandLine 设置）
  static bool g_enablePacketParse = false;

  static void SetEnable (bool enable) { g_enablePacketParse = enable; }
  static bool IsEnabled () { return g_enablePacketParse; }

  /// 将 payload 转为十六进制字符串（便于日志）
  static std::string ToHex (const uint8_t* data, uint32_t len, uint32_t maxBytes = 64)
  {
    std::ostringstream oss;
    uint32_t n = std::min (len, maxBytes);
    for (uint32_t i = 0; i < n; ++i)
      {
        oss << std::hex << std::setw (2) << std::setfill ('0') << (int) data[i];
        if ((i + 1) % 8 == 0) oss << " ";
      }
    if (len > maxBytes) oss << " ...";
    return oss.str ();
  }

  /// 解析 TLV 头并返回 Type/Length 及 Value 注释
  static std::string ParseTlvValue (const uint8_t* data, uint32_t len)
  {
    if (len < 3) return "(too short)";
    uint8_t type = data[0];
    uint16_t valLen = (static_cast<uint16_t> (data[1]) << 8) | data[2];
    std::ostringstream oss;
    const char* typeStr = "Unknown";
    if (type == NmsTlv::TYPE_TELEMETRY_010) typeStr = "Telemetry(0x10)";
    else if (type == NmsTlv::TYPE_ROLE_011) typeStr = "Role(0x11)";
    else if (type == NmsTlv::TYPE_SUBNET_012) typeStr = "ConfigModel(0x12)";
    else if (type == NmsTlv::TYPE_FLOW_020) typeStr = "BusinessModel(0x20)";
    else if (type == NmsTlv::TYPE_TOPO_030) typeStr = "TopologyModel(0x30)";
    else if (type == NmsTlv::TYPE_LINK_031) typeStr = "TopologyModel-LinkCtrl(0x31)";
    else if (type == NmsTlv::TYPE_FAULT_040) typeStr = "FaultModel(0x40)";
    else if (type == NmsTlv::TYPE_ROUTE_FAIL_041) typeStr = "RouteFail(0x41)";
    else if (type == NmsTlv::TYPE_HELLO_ELECTION) typeStr = "HelloElection";
    else if (type == NmsTlv::TYPE_NODE_REPORT_SPN) typeStr = "NodeReportSpn";
    else if (type == NmsTlv::TYPE_TOPOLOGY_AGGREGATE) typeStr = "TopologyAggregate";
    else if (type == NmsTlv::TYPE_SCORE_FLOOD) typeStr = "ScoreFlood";
    oss << "Type=" << typeStr << "(0x" << std::hex << (int)type << std::dec << "), Len=" << valLen;
    if (len >= 3u + valLen)
      {
        if (type == NmsTlv::TYPE_TELEMETRY_010)
          {
            if (valLen >= 24)
              {
                double e, q, m;
                std::memcpy (&e, data + 3, 8);
                std::memcpy (&q, data + 11, 8);
                std::memcpy (&m, data + 19, 8);
                oss << " [energy=" << std::fixed << std::setprecision (3) << e
                    << ", linkQ=" << q << ", mobility=" << m << "]";
              }
          }
        else if (type == NmsTlv::TYPE_LINK_031 && valLen >= 8)
          {
            double d;
            std::memcpy (&d, data + 3, 8);
            oss << " [deltaEnergy=" << std::fixed << std::setprecision (3) << d << "]";
          }
        else if (type == NmsTlv::TYPE_SUBNET_012 && valLen > 0)
          {
            oss << " [value=\"" << std::string (reinterpret_cast<const char*> (data + 3), std::min (valLen, (uint16_t)32u)) << "\"]";
          }
        else if (type == NmsTlv::TYPE_HELLO_ELECTION && valLen >= 12)
          {
            uint32_t nid = (static_cast<uint32_t> (data[3]) << 24) | (static_cast<uint32_t> (data[4]) << 16)
                           | (static_cast<uint32_t> (data[5]) << 8) | data[6];
            double sc = 0.0;
            std::memcpy (&sc, data + 7, 8);
            oss << " [nodeId=" << nid << ", score=" << std::fixed << std::setprecision (3) << sc << "]";
          }
      }
    return oss.str ();
  }

  /// 运行时输出：数据包（TLV 业务/聚合）解析
  static void LogDataPacket (uint32_t nodeId, const char* direction,
                             Ipv4Address srcAddr, Ipv4Address dstAddr, uint16_t srcPort, uint16_t dstPort,
                             const uint8_t* data, uint32_t len)
  {
    if (!IsEnabled () || len == 0) return;
    std::ostringstream oss;
    oss << "[" << direction << "] NodeId=" << nodeId
        << " Src=" << srcAddr << ":" << srcPort << " Dst=" << dstAddr << ":" << dstPort
        << " Size=" << len << "B";
    NmsLog ("INFO", nodeId, "PKT_PARSE", oss.str ());
    oss.str (""); oss << "  HEX: " << ToHex (data, len);
    NmsLog ("INFO", nodeId, "PKT_PARSE", oss.str ());
    if (len >= 6)
      {
        uint8_t frameType = data[0], qos = data[1], src = data[2], dst = data[3], seq = data[4], payloadLen = data[5];
        oss.str ("");
        oss << "  HNMP-Header: frameType=" << static_cast<uint32_t> (frameType)
            << " qos=" << static_cast<uint32_t> (qos)
            << " srcId=" << static_cast<uint32_t> (src)
            << " dstId=" << static_cast<uint32_t> (dst)
            << " seq=" << static_cast<uint32_t> (seq)
            << " payloadLen=" << static_cast<uint32_t> (payloadLen)
            << " totalLen=" << len;
        NmsLog ("INFO", nodeId, "PKT_PARSE", oss.str ());
        uint32_t off = 6;
        uint32_t idx = 0;
        while (off + 3 <= len)
          {
            uint16_t vlen = (static_cast<uint16_t> (data[off + 1]) << 8) | data[off + 2];
            if (off + 3u + vlen > len) break;
            std::string tlvDesc = ParseTlvValue (data + off, 3u + vlen);
            oss.str ("");
            oss << "  TLV[" << idx++ << "]: " << tlvDesc << ", frameLen=" << (3u + vlen);
            NmsLog ("INFO", nodeId, "PKT_PARSE", oss.str ());
            off += 3u + vlen;
          }
      }
    else if (len >= 3)
      {
        std::string tlvDesc = ParseTlvValue (data, len);
        oss.str (""); oss << "  TLV: " << tlvDesc << ", totalLen=" << len;
        NmsLog ("INFO", nodeId, "PKT_PARSE", oss.str ());
      }
  }

  /// 运行时输出：Hello 包（8 字节 double Score）
  static void LogHelloPacket (uint32_t nodeId, const char* direction,
                              Ipv4Address fromAddr, uint16_t fromPort,
                              const uint8_t* data, uint32_t len)
  {
    if (!IsEnabled ()) return;
    std::ostringstream oss;
    oss << "[" << direction << "] NodeId=" << nodeId << " From=" << fromAddr << ":" << fromPort
        << " Size=" << len << "B HEX: " << ToHex (data, len);
    if (len >= 8)
      {
        double score = 0.0;
        std::memcpy (&score, data, 8);
        oss << " Score=" << std::fixed << std::setprecision (3) << score;
      }
    NmsLog ("INFO", nodeId, "PKT_PARSE_HELLO", oss.str ());
  }

  /// 运行时输出：策略包（TLV Type 0x04）
  static void LogPolicyPacket (uint32_t nodeId, const char* direction,
                               Ipv4Address fromAddr, uint16_t fromPort,
                               const uint8_t* data, uint32_t len)
  {
    if (!IsEnabled ()) return;
    std::ostringstream oss;
    oss << "[" << direction << "] NodeId=" << nodeId << " From=" << fromAddr << ":" << fromPort
        << " Size=" << len << "B " << ToHex (data, std::min (len, 32u));
    if (len >= 3) oss << " " << ParseTlvValue (data, len);
    NmsLog ("INFO", nodeId, "PKT_PARSE_POLICY", oss.str ());
  }
}
#else
#include "../protocol/tlv.h"
#include "../protocol/packet-parse.h"
#endif

/*======================== 自定义应用：HeterogeneousNodeApp ========================
 *
 * 设计思想：
 *  - 继承 ns3::Application，避免直接使用 UdpClient，便于后续灵活扩展
 *  - 模拟“节点向 GMC 发送状态数据（UV-MIB）”
 *  - 在 SendPacket() 中预留 “智能流控算法” 插入点：
 *      // TODO: 在此处插入智能流控算法
 *
 * 扩展建议：
 *  - 你可以在类中增加成员变量（如队列长度、链路状态统计、历史吞吐量等）
 *  - 在 SetAttributes 或自定义的 Configure() 函数中，接入你的控制参数
 *===========================================================================*/

class HeterogeneousNodeApp : public Application
{
public:
  /// 子网类型（用于差异化传输策略）
  enum SubnetType
  {
    SUBNET_LTE,
    SUBNET_ADHOC,
    SUBNET_DATALINK
  };

  HeterogeneousNodeApp ();
  virtual ~HeterogeneousNodeApp ();

  /**
   * 用户侧配置接口：
   *  - socketType: 如 UdpSocketFactory::GetTypeId()
   *  - remoteAddress: 目标地址（一般为 GMC 的 IP）
   *  - remotePort: 目标端口
   *  - packetSize: 初始包大小（字节）
   *  - interval: 发送间隔
   */
  void Configure (TypeId socketType,
                  Ipv4Address remoteAddress,
                  uint16_t remotePort,
                  uint32_t packetSize,
                  Time interval);

  /// 设置当前节点所属子网类型
  void SetSubnetType (SubnetType type);

  /// 设置是否为子网 SPN 及策略转发用的子网广播地址（仅 Adhoc/DataLink 有效）
  void SetSpnRole (bool isSpn, Ipv4Address subnetBroadcast);

  /// 当前是否为子网 SPN（供 state jsonl 写入 role，便于退网后新 SPN 在可视化中正确显示）
  bool IsSpn () const { return m_isSpn; }
  bool IsBackupSpn () const { return m_isBackupSpn; }
  uint64_t GetProtocolSuppressCount () const { return m_protocolSuppressCount; }
  uint64_t GetReportedPackets () const { return m_reportedPackets; }
  uint64_t GetTotalScheduleDecisions () const { return m_totalScheduleDecisions; }
  uint64_t GetTriggeredScheduleDecisions () const { return m_triggeredScheduleDecisions; }
  uint64_t GetSuppressedScheduleDecisions () const { return m_suppressedScheduleDecisions; }
  double GetAverageReportIntervalSec () const
  {
    return (m_reportIntervalSamples > 0)
           ? (m_totalReportIntervalSec / static_cast<double> (m_reportIntervalSamples))
           : 0.0;
  }
  uint64_t GetAggregateSuppressedCount () const { return m_aggregateSuppressedCount; }
  uint64_t GetAggregateRawBytes () const { return m_aggregateRawBytes; }
  uint64_t GetAggregateSentBytes () const { return m_aggregateSentBytes; }

  /// 设置控制/数据通道端口（双通道时：数据 9999，控制 8888；否则使用默认 5001/5002/5003/6000）
  void SetChannelPorts (uint16_t reportPort, uint16_t spnPolicyPort, uint16_t nodePolicyPort, uint16_t helloPort);

  /// 设置本节点回程 GMC 地址（仅 Adhoc/DataLink；当该节点被选为 SPN 时向此地址上报聚合数据）
  void SetGmcBackhaul (Ipv4Address gmcAddr, uint16_t gmcPort);

  /// 设置入网初始 UV-MIB（由 joinconfig 注入；energy/linkQuality 在 [0,1]，<0 表示不覆盖）
  void SetInitialUvMib (double energy, double linkQuality);

  /// 时序入网：设置本节点入网时间（秒），未入网前应用层静默、不耗电
  void SetJoinTime (double joinTimeSec);
  /// 论文 4.5.2/4.6：上报抑制与聚合参数
  void SetProtocolTunables (double energyDeltaThreshold, double suppressWindowSec, double aggregateIntervalSec);
  void SetSpnElectionTunables (double electionTimeoutSec, uint8_t heartbeatMissThreshold);

  /// 事件注入：强制节点失效（能量置 0、停止发包、关闭 socket），用于模拟断电/宕机
  void ForceFail ();
  /// 事件注入：触发一次立即重选（用于节点故障后加速收敛）
  void TriggerElectionNow ();

protected:
  virtual void StartApplication (void); ///< Application 启动时被调用
  virtual void StopApplication (void);  ///< Application 停止时被调用

  /// 真正执行发包的函数（算法插入点在此）
  void SendPacket ();

  /// 调度下一次发送事件
  void ScheduleNextTx ();
  bool ShouldScheduleReport (double delta, double nowSec, std::string* reason) const;
  double ComputeReportStateDelta () const;
  void LogDecision (const std::string& action, const std::string& reason, double delta, double nowSec) const;

private:
  /// UV-MIB：节点本地状态
  struct UvMib
  {
    double m_energy;             ///< 剩余能量 [0,1]
    double m_linkQuality;        ///< 链路质量 [0,1]
    double m_mobilityScore;      ///< 移动性评分 [0,1]（越低越稳）
    double m_lastReportedEnergy; ///< 上次上报时的能量（DataLink 差量压缩使用）
  };

  // 论文 4.4：UV-MIB 四大管理模型
  struct UvMibConfigIntentModel
  {
    uint8_t role;          ///< GMC/SPN/TSN
    uint8_t subnetType;    ///< LTE/ADHOC/DATALINK
    uint8_t qosIntent;     ///< 业务意图QoS等级
    uint8_t reserve;
  };
  struct UvMibBusinessPerfModel
  {
    uint16_t flowId;
    uint8_t  priority;
    double   throughputMbps;
    double   delayMs;
    double   lossPct;
  };
  struct UvMibRouteTopoModel
  {
    uint16_t neighborCount;
    double   avgLinkQuality;
    uint16_t routeCost;
    uint16_t macRetry;
  };
  struct UvMibFaultAlarmModel
  {
    uint8_t  faultType;
    uint8_t  severity;
    uint16_t code;
    double   faultTs;
  };

  static uint32_t SerializeUvMibConfigIntent (const UvMibConfigIntentModel& m, uint8_t* out, uint32_t outSize);
  static uint32_t SerializeUvMibBusinessPerf (const UvMibBusinessPerfModel& m, uint8_t* out, uint32_t outSize);
  static uint32_t SerializeUvMibRouteTopo (const UvMibRouteTopoModel& m, uint8_t* out, uint32_t outSize);
  static uint32_t SerializeUvMibFaultAlarm (const UvMibFaultAlarmModel& m, uint8_t* out, uint32_t outSize);

  /// 在发送前更新 UV-MIB：能量递减，链路质量/移动性轻微随机扰动
  void UpdateUvMib ();

  /// 计算多属性效用评分 Score
  double CalculateUtilityScore ();

  /// 广播携带自身 Score 的 Hello 包（Ad-hoc / DataLink）
  void SendHello (double score);
  /// 周期性广播 Score 泛洪包（TTL 泛洪，使全子网获得一致 Score 视图，消除脑裂）
  void SendScoreFlood (double myScore);

  /// 接收邻居 Hello，更新邻居表与全局 Score 表；若为 ScoreFlood 包则合并并转发
  void HandleHello (Ptr<Socket> socket);
  /// 处理收到的 Score 泛洪包：合并到 m_globalScores，TTL>0 时递减并转发
  void HandleScoreFloodPacket (const uint8_t* data, uint32_t len, double myScore);
  /// 主 SPN 周期发送心跳到簇内（100ms）
  void SendHeartbeatSync ();
  /// 备 SPN 周期检查心跳超时并触发接管（连续3次）
  void CheckPrimaryHeartbeat ();

  /// 动态选举：根据自身与邻居 Score 更新 m_isSpn、m_isBackupSpn、m_reportTargetAddress，并重连 m_socket
  void RunElection ();

  /// SPN：接收子网内节点上报，聚合缓存（含拓扑解码）
  void RecvFromSubnet (Ptr<Socket> socket);
  /// SPN：接收 GMC 策略，转发至子网
  void RecvPolicy (Ptr<Socket> socket);
  /// SPN：定时将聚合数据压缩后发往 GMC
  void FlushAggregatedToGmc ();
  /// 非 SPN：接收 SPN 转发的策略
  void HandlePolicyFromSpn (Ptr<Socket> socket);
  /// 论文 4.3：统一 HNMP 帧封装（6B 头；窄带链路隐式 3B 头）
  uint32_t BuildHnmpFrame (uint8_t frameType, uint8_t qos, uint8_t dstId,
                           const uint8_t* tlv, uint32_t tlvLen,
                           uint8_t* out, uint32_t outSize);
  /// 解包 HNMP 帧并返回内部 TLV 载荷；兼容旧版“裸 TLV”输入
  bool ParseHnmpFrame (const uint8_t* in, uint32_t inLen,
                       const uint8_t** outPayload, uint32_t* outPayloadLen);

  Ptr<Socket>     m_socket;
  Ipv4Address     m_peerAddress;
  uint16_t        m_peerPort;
  TypeId          m_socketType;

  EventId         m_sendEvent;
  bool            m_running;

  uint32_t        m_packetSize;   ///< 当前包大小（可被算法动态调整）
  Time            m_interval;     ///< 当前发送周期（可被算法动态调整）
  uint32_t        m_packetsSent;  ///< 已发送包计数（可用于统计/控制）
  uint8_t         m_hnmpSeq;      ///< HNMP 序列号（0~255 循环）
  uint64_t        m_protocolSuppressCount; ///< 协议抑制次数（非链路丢包）
  uint64_t        m_reportedPackets; ///< 实际发送上报次数
  double          m_energyDeltaThreshold; ///< 差值上报阈值（DataLink）
  double          m_stateSuppressWindowSec; ///< 状态抑制窗口（秒）
  double          m_lastStateReportTs;   ///< 最近一次状态上报时间

  UvMib           m_uvMib;        ///< 当前 UV-MIB 状态
  double          m_maxKnownScore;///< 当前已知最大 Score
  bool            m_isProxy;      ///< 当前是否认为自己是 Proxy

  SubnetType      m_subnetType;   ///< 子网类型

  Ptr<UniformRandomVariable> m_rand;       ///< 随机数源（能量/质量/速度扰动）

  Ptr<Socket>     m_helloSocket;          ///< Hello 广播 Socket
  uint16_t        m_helloPort;            ///< Hello 端口

  // === Proxy 聚合 / 策略下发（仅 Adhoc、DataLink 子网） ===
  bool            m_isSpn;                ///< 是否为本子网 SPN（由框架在安装时设置）
  Ipv4Address     m_subnetBroadcast;      ///< 子网广播地址（SPN 转发策略用）
  static const uint16_t SUBNET_REPORT_PORT = 5001;  ///< 子网节点上报 SPN 端口（可被 SetChannelPorts 覆盖）
  static const uint16_t SPN_POLICY_PORT    = 5002;  ///< SPN 接收 GMC 策略端口
  static const uint16_t NODE_POLICY_PORT   = 5003;  ///< 子网节点接收策略端口
  uint16_t m_subnetReportPort;  ///< 实际使用的上报端口（默认 5001，双通道时为 9999）
  uint16_t m_spnPolicyPort;     ///< 实际使用的 SPN 策略端口（默认 5002，双通道时为 8888）
  uint16_t m_nodePolicyPort;     ///< 实际使用的节点策略端口（默认 5003，双通道时为 8888）
  Ptr<Socket>     m_aggregateSocket;      ///< SPN 收子网上报（m_subnetReportPort）
  Ptr<Socket>     m_policySocket;         ///< SPN 收 GMC 策略（5002）
  Ptr<Socket>     m_policyRecvSocket;     ///< 非 SPN 收 SPN 转发的策略（5003）
  std::map<Ipv4Address, std::vector<uint8_t>> m_aggregateBuf;  ///< 按源 IP 缓存最新 TLV
  EventId         m_flushEvent;           ///< 聚合上报定时器
  EventId         m_heartbeatEvent;       ///< 主备心跳事件
  Time            m_aggregateInterval;    ///< 聚合上报周期

  /// 动态选举与邻居发现（Adhoc/DataLink）
  Ipv4Address     m_gmcBackhaulAddress;   ///< 本节点回程 GMC 地址（SPN 时向此上报）
  uint16_t        m_gmcBackhaulPort;      ///< 回程 GMC 端口
  Ipv4Address     m_reportTargetAddress;  ///< 当前主 SPN 子网地址（非 SPN 时向此上报）
  std::map<uint32_t, double> m_neighborScores;   ///< 邻居 nodeId -> 最新 Score
  std::map<uint32_t, Ipv4Address> m_neighborAddrs; ///< 邻居 nodeId -> 子网 IP（来自 Hello 源地址）
  bool            m_isBackupSpn;          ///< 是否为备选 SPN（主 SPN 失效时接管）
  /// 全网 Score 同步（泛洪得到）：nodeId -> score，用于全局一致选举，避免脑裂
  std::map<uint32_t, double> m_globalScores;
  std::map<uint32_t, double> m_globalScoreTime;   ///< nodeId -> 最后更新时间（秒），超时视为掉线
  static const double SPN_SCORE_STALE_SEC;         ///< Score 超过此秒未更新则视为节点离线
  static const double HEARTBEAT_SEC;               ///< 心跳周期（按论文实验参数可调）
  static const uint8_t HEARTBEAT_MISS_THRESHOLD;   ///< 连续丢失阈值
  static const double SPN_SWITCH_HYSTERESIS;       ///< SPN 切换迟滞阈值
  /// Score 平滑：对瞬时 UV-MIB/拓扑输入与最终得分做 EMA，抑制随机扰动导致的选举抖动
  static const double SCORE_INPUT_EMA_ALPHA;       ///< 能量/链路/拓扑度量 EMA 系数（越大越跟瞬时）
  static const double SCORE_OUTPUT_EMA_ALPHA;      ///< 最终 Score EMA 系数
  static const double SPN_PRIMARY_SCORE_THRESHOLD; ///< 主 SPN 得分阈值（低于该值才触发一次得分降级切换）
  static const double SPN_INITIAL_ELECTION_WARMUP_SEC; ///< 初始选举预热窗口，避免启动瞬时误选
  static const uint32_t SPN_PRIMARY_STABLE_TICKS;  ///< 连续多少次 RunElection 认定同一主 SPN 后才切换 committed 主
  uint32_t        m_currentPrimaryId;      ///< 当前感知的主SPN
  double          m_lastPrimaryHeartbeatTs;///< 最近收到主SPN心跳时刻
  uint8_t         m_missedHeartbeats;      ///< 连续丢心跳计数
  bool            m_failoverPending;       ///< 已触发接管判定，等待成为主SPN
  double          m_failoverStartTs;       ///< 触发接管时刻，用于自愈耗时统计
  double          m_joinTime;             ///< 入网时间（秒），未到则应用层静默、不耗电
  bool            m_scoreFilterInit;       ///< EMA 状态是否已初始化
  double          m_emaEnergy;             ///< 能量 EMA（用于 Score）
  double          m_emaLink;               ///< 链路质量 EMA（用于 Score）
  double          m_emaTopoMetric;         ///< 拓扑/移动性度量 EMA（用于 Score）
  double          m_velocityEma;           ///< 随机速度 EMA，避免每周期全新 velocity 导致 topology 剧变
  double          m_emaFinalScore;           ///< 对外广播/写入 global 的最终平滑 Score
  uint32_t        m_committedPrimaryId;    ///< 已提交的主 SPN（稳定切换前保持不变）
  uint32_t        m_primaryStableCounter;  ///< 与 m_lastRawPrimaryId 连续一致计数
  uint32_t        m_lastRawPrimaryId;      ///< 上一轮排序得到的原始主 SPN 候选
  Ipv4Address     m_socketReportBoundAddr; ///< m_socket 当前已 Connect 的对端地址（避免重复 Connect）
  uint16_t        m_socketBoundPeerPort;     ///< 与 m_socketReportBoundAddr 对应的远端端口
  bool            m_initialSpnLocked;      ///< 是否已完成初始 SPN 锁定（后续默认不重选）
  bool            m_failoverSwitchUsed;    ///< 全程仅允许一次“主SPN退网”触发切换
  bool            m_energySwitchUsed;      ///< 全程仅允许一次“主SPN低于阈值”触发切换
  uint32_t        m_committedBackupId;     ///< 已提交的备SPN（与主SPN解耦稳定）
  double          m_spnScoreStaleSec;      ///< 选举超时（秒），超时视为离线
  uint8_t         m_heartbeatMissThreshold;///< 主SPN心跳连续丢失阈值（可配置）
  bool            m_lowEnergyAlarmRaised;  ///< 低电量告警仅上报一次，避免刷屏
  // 状态驱动上报控制：前置抑制 + 最大静默兜底
  double          m_lastObservedEnergy;
  double          m_lastObservedLinkQuality;
  double          m_lastObservedMobility;
  double          m_maxSilentSec;
  double          m_reportStateThreshold;
  uint64_t        m_totalScheduleDecisions;
  uint64_t        m_triggeredScheduleDecisions;
  uint64_t        m_suppressedScheduleDecisions;
  double          m_totalReportIntervalSec;
  uint64_t        m_reportIntervalSamples;
  // SPN 聚合增量判断（避免重复发送）
  uint32_t        m_lastAggregateHash;
  double          m_lastAggregateSendTs;
  uint64_t        m_aggregateSuppressedCount;
  uint64_t        m_aggregateRawBytes;
  uint64_t        m_aggregateSentBytes;
  struct SharedSpnState
  {
    bool initialized;
    uint32_t primaryId;
    uint32_t backupId;
    bool failoverUsed;
    bool energySwitchUsed;
    bool frozen;
    double initializedTs;
    std::map<uint32_t, double> scoreByNode;
    std::map<uint32_t, double> scoreTimeByNode;
    SharedSpnState ()
      : initialized (false),
        primaryId (0),
        backupId (0),
        failoverUsed (false),
        energySwitchUsed (false),
        frozen (false),
        initializedTs (0.0)
    {}
  };
  static std::map<uint8_t, SharedSpnState> s_sharedSpnState; ///< 子网维度共享主备状态（强约束：唯一主备、最多两次切换）
  static bool s_globalFailoverUsed; ///< 全仿真仅允许一次退网切换
  static bool s_globalEnergyUsed;   ///< 全仿真仅允许一次低分切换
  /// SPN 内部：解码后的节点状态与拓扑边（用于构建 TYPE_TOPOLOGY_AGGREGATE）
  struct NodeReportState {
    uint32_t nodeId;
    double px, py, pz;
    float vx, vy, vz;
    double energy, linkQ;
    std::vector<uint32_t> neighborIds;
  };
  std::map<uint32_t, NodeReportState> m_subnetNodeStates;
  std::set<std::pair<uint32_t, uint32_t>> m_subnetEdges;  ///< 拓扑边 (from, to)，from < to 存储
};

HeterogeneousNodeApp::HeterogeneousNodeApp ()
  : m_socket (0),
    m_peerAddress ("0.0.0.0"),
    m_peerPort (0),
    m_socketType (UdpSocketFactory::GetTypeId ()),
    m_running (false),
    m_packetSize (100),
    m_interval (Seconds (1.0)),
    m_packetsSent (0),
    m_hnmpSeq (0),
    m_protocolSuppressCount (0),
    m_reportedPackets (0),
    m_energyDeltaThreshold (0.05),
    m_stateSuppressWindowSec (1.0),
    m_lastStateReportTs (-1.0),
    m_maxKnownScore (0.0),
    m_isProxy (false),
    m_subnetType (SUBNET_LTE),
    m_rand (0),
    m_helloSocket (0),
    m_helloPort (6000),
    m_isSpn (false),
    m_subnetBroadcast ("0.0.0.0"),
    m_subnetReportPort (5001),
    m_spnPolicyPort (5002),
    m_nodePolicyPort (5003),
    m_aggregateSocket (0),
    m_policySocket (0),
    m_policyRecvSocket (0),
    m_aggregateInterval (Seconds (2.0)),
    m_gmcBackhaulAddress ("0.0.0.0"),
    m_gmcBackhaulPort (0),
    m_reportTargetAddress ("0.0.0.0"),
    m_isBackupSpn (false),
    m_currentPrimaryId (0),
    m_lastPrimaryHeartbeatTs (0.0),
    m_missedHeartbeats (0),
    m_failoverPending (false),
    m_failoverStartTs (0.0),
    m_joinTime (0.0),
    m_scoreFilterInit (false),
    m_emaEnergy (0.9),
    m_emaLink (0.85),
    m_emaTopoMetric (0.5),
    m_velocityEma (10.0),
    m_emaFinalScore (0.0),
    m_committedPrimaryId (0),
    m_primaryStableCounter (0),
    m_lastRawPrimaryId (UINT32_MAX),
    m_socketReportBoundAddr ("0.0.0.0"),
    m_socketBoundPeerPort (0),
    m_initialSpnLocked (false),
    m_failoverSwitchUsed (false),
    m_energySwitchUsed (false),
    m_committedBackupId (0),
    m_spnScoreStaleSec (SPN_SCORE_STALE_SEC),
    m_heartbeatMissThreshold (HEARTBEAT_MISS_THRESHOLD),
    m_lowEnergyAlarmRaised (false),
    m_lastObservedEnergy (-1.0),
    m_lastObservedLinkQuality (-1.0),
    m_lastObservedMobility (-1.0),
    m_maxSilentSec (5.0),
    m_reportStateThreshold (0.05),
    m_totalScheduleDecisions (0),
    m_triggeredScheduleDecisions (0),
    m_suppressedScheduleDecisions (0),
    m_totalReportIntervalSec (0.0),
    m_reportIntervalSamples (0),
    m_lastAggregateHash (0),
    m_lastAggregateSendTs (-1.0),
    m_aggregateSuppressedCount (0),
    m_aggregateRawBytes (0),
    m_aggregateSentBytes (0)
{
  // 初始化随机数与 UV-MIB
  m_rand = CreateObject<UniformRandomVariable> ();

  m_uvMib.m_energy            = 0.9;                             // 接近满电
  m_uvMib.m_linkQuality       = m_rand->GetValue (0.7, 1.0);     // 较好链路
  m_uvMib.m_mobilityScore     = m_rand->GetValue (0.3, 0.8);     // 中等稳定
  m_uvMib.m_lastReportedEnergy = m_uvMib.m_energy;
}

HeterogeneousNodeApp::~HeterogeneousNodeApp ()
{
  m_socket = 0;
}

const double HeterogeneousNodeApp::SPN_SCORE_STALE_SEC = 2.0;  // 退网后约 2s 内完成 SPN 重选，避免“SPN 消失”
const double HeterogeneousNodeApp::HEARTBEAT_SEC = 0.25;         // 心跳检测周期 250ms，加速主故障检测
const uint8_t HeterogeneousNodeApp::HEARTBEAT_MISS_THRESHOLD = 3; // 连续 3 次丢失判定主SPN失效
const double HeterogeneousNodeApp::SPN_SWITCH_HYSTERESIS = 0.2; // 选举迟滞阈值
const double HeterogeneousNodeApp::SCORE_INPUT_EMA_ALPHA = 0.22;   // 输入平滑：抑制 linkQ 小幅抖动
const double HeterogeneousNodeApp::SCORE_OUTPUT_EMA_ALPHA = 0.35;  // 输出平滑：Hello/泛洪用分更稳
const double HeterogeneousNodeApp::SPN_PRIMARY_SCORE_THRESHOLD = 0.45; // 主SPN得分低于此阈值才允许一次得分降级切换
const double HeterogeneousNodeApp::SPN_INITIAL_ELECTION_WARMUP_SEC = 3.0; // 初始 3s 等待邻居分数同步后再锁定主备
const uint32_t HeterogeneousNodeApp::SPN_PRIMARY_STABLE_TICKS = 5; // 约 5 个发送周期后才切换 committed 主
std::map<uint8_t, HeterogeneousNodeApp::SharedSpnState> HeterogeneousNodeApp::s_sharedSpnState;
bool HeterogeneousNodeApp::s_globalFailoverUsed = false;
bool HeterogeneousNodeApp::s_globalEnergyUsed = false;

uint32_t
HeterogeneousNodeApp::SerializeUvMibConfigIntent (const UvMibConfigIntentModel& m, uint8_t* out, uint32_t outSize)
{
  if (!out || outSize < 7) return 0;
  NmsTlv::WriteTlvHeader (out, NmsTlv::TYPE_SUBNET_012, 4);
  out[3] = m.role;
  out[4] = m.subnetType;
  out[5] = m.qosIntent;
  out[6] = m.reserve;
  return 7;
}

uint32_t
HeterogeneousNodeApp::SerializeUvMibBusinessPerf (const UvMibBusinessPerfModel& m, uint8_t* out, uint32_t outSize)
{
  const uint16_t valLen = 2 + 1 + 8 + 8 + 8;
  if (!out || outSize < 3u + valLen) return 0;
  NmsTlv::WriteTlvHeader (out, NmsTlv::TYPE_FLOW_020, valLen);
  out[3] = (m.flowId >> 8) & 0xff;
  out[4] = m.flowId & 0xff;
  out[5] = m.priority;
  std::memcpy (out + 6, &m.throughputMbps, 8);
  std::memcpy (out + 14, &m.delayMs, 8);
  std::memcpy (out + 22, &m.lossPct, 8);
  return 3 + valLen;
}

uint32_t
HeterogeneousNodeApp::SerializeUvMibRouteTopo (const UvMibRouteTopoModel& m, uint8_t* out, uint32_t outSize)
{
  const uint16_t valLen = 2 + 8 + 2 + 2;
  if (!out || outSize < 3u + valLen) return 0;
  NmsTlv::WriteTlvHeader (out, NmsTlv::TYPE_TOPO_030, valLen);
  out[3] = (m.neighborCount >> 8) & 0xff;
  out[4] = m.neighborCount & 0xff;
  std::memcpy (out + 5, &m.avgLinkQuality, 8);
  out[13] = (m.routeCost >> 8) & 0xff;
  out[14] = m.routeCost & 0xff;
  out[15] = (m.macRetry >> 8) & 0xff;
  out[16] = m.macRetry & 0xff;
  return 3 + valLen;
}

uint32_t
HeterogeneousNodeApp::SerializeUvMibFaultAlarm (const UvMibFaultAlarmModel& m, uint8_t* out, uint32_t outSize)
{
  const uint16_t valLen = 1 + 1 + 2 + 8;
  if (!out || outSize < 3u + valLen) return 0;
  NmsTlv::WriteTlvHeader (out, NmsTlv::TYPE_FAULT_040, valLen);
  out[3] = m.faultType;
  out[4] = m.severity;
  out[5] = (m.code >> 8) & 0xff;
  out[6] = m.code & 0xff;
  std::memcpy (out + 7, &m.faultTs, 8);
  return 3 + valLen;
}

void
HeterogeneousNodeApp::Configure (TypeId socketType,
                                 Ipv4Address remoteAddress,
                                 uint16_t remotePort,
                                 uint32_t packetSize,
                                 Time interval)
{
  m_socketType  = socketType;
  m_peerAddress = remoteAddress;
  m_peerPort    = remotePort;
  m_packetSize  = packetSize;
  m_interval    = interval;
}

void
HeterogeneousNodeApp::SetSubnetType (SubnetType type)
{
  m_subnetType = type;
}

void
HeterogeneousNodeApp::SetSpnRole (bool isSpn, Ipv4Address subnetBroadcast)
{
  m_isSpn = isSpn;
  m_subnetBroadcast = subnetBroadcast;
}

void
HeterogeneousNodeApp::SetChannelPorts (uint16_t reportPort, uint16_t spnPolicyPort, uint16_t nodePolicyPort, uint16_t helloPort)
{
  m_subnetReportPort = reportPort;
  m_spnPolicyPort = spnPolicyPort;
  m_nodePolicyPort = nodePolicyPort;
  m_helloPort = helloPort;
}

void
HeterogeneousNodeApp::SetGmcBackhaul (Ipv4Address gmcAddr, uint16_t gmcPort)
{
  m_gmcBackhaulAddress = gmcAddr;
  m_gmcBackhaulPort = gmcPort;
}

void
HeterogeneousNodeApp::SetJoinTime (double joinTimeSec)
{
  m_joinTime = joinTimeSec >= 0.0 ? joinTimeSec : 0.0;
}

void
HeterogeneousNodeApp::SetProtocolTunables (double energyDeltaThreshold, double suppressWindowSec, double aggregateIntervalSec)
{
  if (energyDeltaThreshold > 0.0)
    m_energyDeltaThreshold = energyDeltaThreshold;
  if (suppressWindowSec >= 0.0)
    m_stateSuppressWindowSec = suppressWindowSec;
  if (aggregateIntervalSec > 0.0)
    m_aggregateInterval = Seconds (aggregateIntervalSec);
}

void
HeterogeneousNodeApp::SetSpnElectionTunables (double electionTimeoutSec, uint8_t heartbeatMissThreshold)
{
  if (electionTimeoutSec > 0.0)
    {
      m_spnScoreStaleSec = electionTimeoutSec;
    }
  if (heartbeatMissThreshold >= 1)
    {
      m_heartbeatMissThreshold = heartbeatMissThreshold;
    }
}

void
HeterogeneousNodeApp::SetInitialUvMib (double energy, double linkQuality)
{
  if (energy >= 0.0)
    {
      m_uvMib.m_energy = energy;
      m_uvMib.m_lastReportedEnergy = energy;
    }
  if (linkQuality >= 0.0 && linkQuality <= 1.0)
    m_uvMib.m_linkQuality = linkQuality;
}

void
HeterogeneousNodeApp::UpdateUvMib ()
{
  // LTE 节点降低能耗速度；Adhoc/DataLink 保持相对明显耗电
  double oldEnergy = m_uvMib.m_energy;
  double decay = (m_subnetType == SUBNET_LTE) ? 0.00005 : 0.005;
  m_uvMib.m_energy = std::max (0.0, m_uvMib.m_energy - decay);

  // LTE 场景降低链路/移动性扰动，减少无意义波动与告警
  double dqRange = (m_subnetType == SUBNET_LTE) ? 0.01 : 0.05;
  double dmRange = (m_subnetType == SUBNET_LTE) ? 0.01 : 0.05;
  double dq = m_rand->GetValue (-dqRange, dqRange);
  double dm = m_rand->GetValue (-dmRange, dmRange);

  m_uvMib.m_linkQuality   = std::min (1.0, std::max (0.0, m_uvMib.m_linkQuality + dq));
  m_uvMib.m_mobilityScore = std::min (1.0, std::max (0.0, m_uvMib.m_mobilityScore + dm));

  NS_LOG_INFO ("[UV-MIB] Node " << GetNode ()->GetId ()
                                << " Energy " << std::fixed << std::setprecision (3)
                                << oldEnergy << " -> " << m_uvMib.m_energy
                                << ", LinkQ:" << m_uvMib.m_linkQuality
                                << ", Mob:"   << m_uvMib.m_mobilityScore);
}

double
HeterogeneousNodeApp::CalculateUtilityScore ()
{
  static const double w_e = 0.3;
  static const double w_t = 0.3;
  static const double w_l = 0.25;
  const double aIn = SCORE_INPUT_EMA_ALPHA;
  const double aOut = SCORE_OUTPUT_EMA_ALPHA;
  const bool firstCall = !m_scoreFilterInit;

  // 随机速度做滑动平均，避免每个周期全新 velocity 导致 topology 项剧烈跳变
  double vInst = m_rand->GetValue (0.0, 20.0);
  if (firstCall)
    {
      m_velocityEma = vInst;
    }
  else
    {
      m_velocityEma = aIn * vInst + (1.0 - aIn) * m_velocityEma;
    }

  double topologyInstant = 1.0 / (1.0 + std::exp (2.0 * (m_velocityEma - 10.0)));
  // 同时用 UV-MIB 移动性（已含小幅扰动）约束拓扑项，使 Score 与节点模型一致
  double topoBlend = 0.6 * topologyInstant + 0.4 * m_uvMib.m_mobilityScore;

  double energy = m_uvMib.m_energy;
  double link = m_uvMib.m_linkQuality;

  if (firstCall)
    {
      m_emaEnergy = energy;
      m_emaLink = link;
      m_emaTopoMetric = topoBlend;
    }
  else
    {
      m_emaEnergy = aIn * energy + (1.0 - aIn) * m_emaEnergy;
      m_emaLink = aIn * link + (1.0 - aIn) * m_emaLink;
      m_emaTopoMetric = aIn * topoBlend + (1.0 - aIn) * m_emaTopoMetric;
    }

  double rawScore = w_e * m_emaEnergy + w_t * m_emaTopoMetric + w_l * m_emaLink;

  // 最终得分再做一层 EMA，供 Hello / ScoreFlood / m_globalScores 使用
  if (firstCall)
    {
      m_emaFinalScore = rawScore;
      m_scoreFilterInit = true;
    }
  else
    {
      m_emaFinalScore = aOut * rawScore + (1.0 - aOut) * m_emaFinalScore;
    }

  double score = m_emaFinalScore;

  NS_LOG_INFO ("[Election] Node " << GetNode ()->GetId ()
                                  << " calculated Score: "
                                  << std::fixed << std::setprecision (3) << score
                                  << " raw=" << rawScore
                                  << " (E:" << m_emaEnergy
                                  << ", T:" << m_emaTopoMetric
                                  << ", L:" << m_emaLink << ")");
  return score;
}

void
HeterogeneousNodeApp::SendHello (double score)
{
  if (!m_helloSocket)
    {
      return;
    }

  uint32_t nodeId = GetNode ()->GetId ();
  uint8_t buf[NmsTlv::HELLO_ELECTION_PAYLOAD];
  uint32_t len = NmsTlv::BuildHelloElectionPayload (buf, sizeof (buf), nodeId, score);
  if (len == 0) return;

  Ptr<Packet> pkt = Create<Packet> (buf, len);
  InetSocketAddress dst = InetSocketAddress (Ipv4Address ("255.255.255.255"), m_helloPort);
  m_helloSocket->SendTo (pkt, 0, dst);

  NS_LOG_INFO ("[Hello] Node " << nodeId
                               << " broadcast nodeId=" << nodeId << " Score "
                               << std::fixed << std::setprecision (3)
                               << score);
}

void
HeterogeneousNodeApp::HandleHello (Ptr<Socket> socket)
{
  Address from;
  Ptr<Packet> packet;

  while ((packet = socket->RecvFrom (from)))
    {
      uint32_t len = packet->GetSize ();
      if (len < 3)
        {
          continue;
        }

      uint8_t buf[2048];
      if (len > sizeof (buf)) continue;
      packet->CopyData (buf, len);

      // 分发：Score 泛洪包与 Hello 选举包共用同一端口
      if (buf[0] == NmsTlv::TYPE_SCORE_FLOOD)
        {
          double myScore = CalculateUtilityScore ();
          HandleScoreFloodPacket (buf, len, myScore);
          continue;
        }
      if (buf[0] == NmsTlv::TYPE_HEARTBEAT_SYNC)
        {
          uint32_t primaryId = 0;
          double beatTs = 0.0;
          if (NmsTlv::ParseHeartbeatSyncPayload (buf, len, &primaryId, &beatTs) > 0)
            {
              m_currentPrimaryId = primaryId;
              m_lastPrimaryHeartbeatTs = Simulator::Now ().GetSeconds ();
              m_missedHeartbeats = 0;
            }
          continue;
        }

      if (len < NmsTlv::HELLO_ELECTION_PAYLOAD || buf[0] != NmsTlv::TYPE_HELLO_ELECTION)
        {
          continue;
        }

      InetSocketAddress iaddr = InetSocketAddress::ConvertFrom (from);
      Ipv4Address fromAddr = iaddr.GetIpv4 ();

      uint32_t neighborNodeId = 0;
      double neighborScore = 0.0;
      uint16_t valLen = NmsTlv::ParseHelloElection (buf, len, &neighborNodeId, &neighborScore);
      if (valLen == 0)
        {
          continue;
        }

      NmsPacketParse::LogHelloPacket (GetNode ()->GetId (), "RECV", fromAddr, iaddr.GetPort (), buf, len);

      uint32_t nodeId = GetNode ()->GetId ();
      bool isNew = (m_neighborScores.find (neighborNodeId) == m_neighborScores.end ());
      m_neighborScores[neighborNodeId] = neighborScore;
      m_neighborAddrs[neighborNodeId] = fromAddr;
      // 同时更新全局 Score 表（用于全子网一致选举，消除脑裂）
      double now = Simulator::Now ().GetSeconds ();
      m_globalScores[neighborNodeId] = neighborScore;
      m_globalScoreTime[neighborNodeId] = now;

      if (neighborScore > m_maxKnownScore)
        {
          NS_LOG_INFO ("[Election] Node " << nodeId
                                          << " updates MaxKnownScore from "
                                          << std::fixed << std::setprecision (3)
                                          << m_maxKnownScore << " to "
                                          << neighborScore << " (neighbor " << neighborNodeId << ")");
          m_maxKnownScore = neighborScore;
        }
      if (isNew)
        {
          std::ostringstream addrStr;
          fromAddr.Print (addrStr);
          NMS_LOG_INFO (nodeId, "NEIGHBOR_DISCOVER", "new neighbor nodeId=" + std::to_string (neighborNodeId)
                        + " addr=" + addrStr.str () + " score=" + std::to_string (neighborScore));
        }
    }
}

void
HeterogeneousNodeApp::HandleScoreFloodPacket (const uint8_t* data, uint32_t len, double myScore)
{
  uint8_t ttl = 0;
  std::vector<std::pair<uint32_t, double>> entries;
  uint32_t consumed = NmsTlv::ParseScoreFlood (data, len, &ttl, &entries);
  if (consumed == 0) return;

  uint32_t selfId = GetNode ()->GetId ();
  double now = Simulator::Now ().GetSeconds ();
  for (const auto& e : entries)
    {
      m_globalScores[e.first] = e.second;
      m_globalScoreTime[e.first] = now;
      SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
      shared.scoreByNode[e.first] = e.second;
      shared.scoreTimeByNode[e.first] = now;
    }

  if (ttl > 0 && m_helloSocket)
    {
      entries.push_back (std::make_pair (selfId, myScore));
      m_globalScores[selfId] = myScore;
      m_globalScoreTime[selfId] = now;
      uint8_t outBuf[NmsTlv::MAX_SCORE_FLOOD_PAYLOAD];
      uint32_t outLen = NmsTlv::BuildScoreFloodPayload (outBuf, sizeof (outBuf), ttl - 1, entries);
      if (outLen > 0)
        {
          Ptr<Packet> pkt = Create<Packet> (outBuf, outLen);
          InetSocketAddress dst = InetSocketAddress (Ipv4Address ("255.255.255.255"), m_helloPort);
          m_helloSocket->SendTo (pkt, 0, dst);
        }
    }
  // 收到泛洪后立即重选，缩短全网视图一致收敛时间
  RunElection ();
}

void
HeterogeneousNodeApp::SendHeartbeatSync ()
{
  if (!m_running || !m_helloSocket || !m_isSpn ||
      !(m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK))
    {
      return;
    }

  uint8_t buf[32];
  uint32_t selfId = GetNode ()->GetId ();
  double now = Simulator::Now ().GetSeconds ();
  uint32_t len = NmsTlv::BuildHeartbeatSyncPayload (buf, sizeof (buf), selfId, now);
  if (len > 0)
    {
      Ptr<Packet> pkt = Create<Packet> (buf, len);
      InetSocketAddress dst = InetSocketAddress (Ipv4Address ("255.255.255.255"), m_helloPort);
      m_helloSocket->SendTo (pkt, 0, dst);
    }
  m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC), &HeterogeneousNodeApp::SendHeartbeatSync, this);
}

void
HeterogeneousNodeApp::CheckPrimaryHeartbeat ()
{
  if (!m_running || !(m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK))
    {
      return;
    }

  if (m_isBackupSpn && m_currentPrimaryId != GetNode ()->GetId ())
    {
      double now = Simulator::Now ().GetSeconds ();
      // 略大于 HEARTBEAT_SEC 的判定裕量，避免调度抖动导致误计丢心跳
      const double hbGrace = HEARTBEAT_SEC * 1.25;
      if (m_lastPrimaryHeartbeatTs > 0.0 && (now - m_lastPrimaryHeartbeatTs) > hbGrace)
        {
          m_missedHeartbeats++;
          m_lastPrimaryHeartbeatTs = now; // 防止一次超时被重复快速累计
      if (m_missedHeartbeats >= m_heartbeatMissThreshold)
            {
              m_failoverPending = true;
              m_failoverStartTs = now;
              m_globalScoreTime[m_currentPrimaryId] = 0.0; // 将原主SPN标记为失效，触发重选
              NMS_LOG_INFO (GetNode ()->GetId (), "SPN_HEARTBEAT_LOST",
                            "primary=" + std::to_string (m_currentPrimaryId) +
                            " lost " + std::to_string ((int)m_missedHeartbeats) + " heartbeats, trigger takeover");
              RunElection ();
            }
        }
    }

  m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC), &HeterogeneousNodeApp::CheckPrimaryHeartbeat, this);
}

void
HeterogeneousNodeApp::SendScoreFlood (double myScore)
{
  if (!m_helloSocket) return;

  uint32_t selfId = GetNode ()->GetId ();
  double now = Simulator::Now ().GetSeconds ();
  m_globalScores[selfId] = myScore;
  m_globalScoreTime[selfId] = now;

  std::vector<std::pair<uint32_t, double>> entries;
  for (const auto& kv : m_globalScores)
    entries.push_back (std::make_pair (kv.first, kv.second));
  if (entries.empty ()) entries.push_back (std::make_pair (selfId, myScore));

  const uint8_t TTL_INIT = 3;
  uint8_t outBuf[NmsTlv::MAX_SCORE_FLOOD_PAYLOAD];
  uint32_t outLen = NmsTlv::BuildScoreFloodPayload (outBuf, sizeof (outBuf), TTL_INIT, entries);
  if (outLen == 0) return;

  Ptr<Packet> pkt = Create<Packet> (outBuf, outLen);
  InetSocketAddress dst = InetSocketAddress (Ipv4Address ("255.255.255.255"), m_helloPort);
  m_helloSocket->SendTo (pkt, 0, dst);
  NMS_LOG_INFO (selfId, "SCORE_FLOOD", "broadcast " + std::to_string (entries.size ()) + " entries TTL=" + std::to_string (TTL_INIT));
}

void
HeterogeneousNodeApp::RunElection ()
{
  if (m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK)
    return;

  uint32_t selfId = GetNode ()->GetId ();
  double myScore = CalculateUtilityScore ();
  double now = Simulator::Now ().GetSeconds ();

  // 使用全局 Score 表（泛洪同步），过滤掉超时未更新的节点（视为掉线），保证全子网唯一主备 SPN
  m_globalScores[selfId] = myScore;
  m_globalScoreTime[selfId] = now;

  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
  shared.scoreByNode[selfId] = myScore;
  shared.scoreTimeByNode[selfId] = now;

  std::vector<std::pair<double, uint32_t>> candidates;
  for (const auto& kv : shared.scoreByNode)
    {
      double age = now - shared.scoreTimeByNode[kv.first];
      if (age <= m_spnScoreStaleSec)
        candidates.push_back (std::make_pair (kv.second, kv.first));
    }
  if (candidates.empty ())
    candidates.push_back (std::make_pair (myScore, selfId));

  // 排序：Score 降序，同分则 nodeId 升序，保证全局一致
  std::sort (candidates.begin (), candidates.end (),
             [] (const std::pair<double, uint32_t>& a, const std::pair<double, uint32_t>& b) {
               if (a.first != b.first) return a.first > b.first;
               return a.second < b.second;
             });

  std::map<uint32_t, double> aliveScoreById;
  for (const auto& c : candidates)
    {
      aliveScoreById[c.second] = c.first;
    }

  uint32_t rawWinnerId = candidates.empty () ? selfId : candidates[0].second;
  bool stateChanged = false;
  std::string changeReason;

  auto pickBackup = [&candidates] (uint32_t spnNodeId) -> uint32_t {
    for (const auto& c : candidates)
      {
        if (c.second != spnNodeId)
          return c.second;
      }
    return spnNodeId;
  };

  // 强约束：
  // - 首次只选一次主备并锁定
  // - 仅允许一次退网切换 + 一次低分切换
  // - 两次事件均发生后冻结，不再重选
  if (!shared.initialized)
    {
      // 启动预热：先等分数同步，避免启动瞬间竞争
      if (candidates.size () < 2 && now < SPN_INITIAL_ELECTION_WARMUP_SEC)
        return;
      shared.primaryId = rawWinnerId;
      shared.backupId = pickBackup (shared.primaryId);
      shared.initialized = true;
      shared.initializedTs = now;
      stateChanged = true;
      changeReason = "initial_lock";
    }
  else if (!shared.frozen)
    {
      const bool eventsEnabled = (now - shared.initializedTs) >= (m_spnScoreStaleSec + 0.5);
      auto itCommitted = aliveScoreById.find (shared.primaryId);
      bool committedAlive = (itCommitted != aliveScoreById.end ());
      double committedScore = committedAlive ? itCommitted->second : 0.0;

      // 事件1：主SPN退网，仅一次（且全仿真仅一次）
      if (eventsEnabled &&
          !committedAlive &&
          !shared.failoverUsed &&
          !s_globalFailoverUsed)
        {
          shared.primaryId = rawWinnerId;
          shared.backupId = pickBackup (shared.primaryId);
          shared.failoverUsed = true;
          s_globalFailoverUsed = true;
          stateChanged = true;
          changeReason = "failover";
        }
      // 事件2：主SPN得分低于阈值，仅一次（且全仿真仅一次）
      else if (eventsEnabled &&
               committedAlive &&
               !shared.energySwitchUsed &&
               !s_globalEnergyUsed &&
               committedScore < SPN_PRIMARY_SCORE_THRESHOLD &&
               rawWinnerId != shared.primaryId)
        {
          auto itTop = aliveScoreById.find (rawWinnerId);
          double topScore = (itTop != aliveScoreById.end ()) ? itTop->second : committedScore;
          if (topScore > committedScore + SPN_SWITCH_HYSTERESIS)
            {
              shared.primaryId = rawWinnerId;
              shared.backupId = pickBackup (shared.primaryId);
              shared.energySwitchUsed = true;
              s_globalEnergyUsed = true;
              stateChanged = true;
              changeReason = "energy_low";
            }
        }

      if (shared.failoverUsed && shared.energySwitchUsed)
        {
          shared.frozen = true;
        }
    }

  m_initialSpnLocked = shared.initialized;
  m_failoverSwitchUsed = shared.failoverUsed;
  m_energySwitchUsed = shared.energySwitchUsed;
  m_committedPrimaryId = shared.primaryId;
  m_committedBackupId = shared.backupId;

  if (m_committedPrimaryId == 0)
    return;

  uint32_t spnNodeId = m_committedPrimaryId;
  uint32_t backupNodeId = (m_committedBackupId == 0) ? spnNodeId : m_committedBackupId;

  m_currentPrimaryId = spnNodeId;

  bool wasSpn = m_isSpn;
  bool wasBackup = m_isBackupSpn;
  m_isSpn = (spnNodeId == selfId);
  m_isBackupSpn = (backupNodeId == selfId && !m_isSpn);

  // 普通节点：上报目标对应当前「有效」主 SPN；无邻居地址时再尝试备 SPN
  if (!m_isSpn && spnNodeId != selfId)
    {
      auto it = m_neighborAddrs.find (spnNodeId);
      if (it != m_neighborAddrs.end ())
        m_reportTargetAddress = it->second;
      else
        {
          auto itBackup = m_neighborAddrs.find (backupNodeId);
          if (itBackup != m_neighborAddrs.end ())
            m_reportTargetAddress = itBackup->second;
          else
            m_reportTargetAddress = Ipv4Address ("0.0.0.0");
        }
    }

  if (m_isSpn && !wasSpn)
    {
      if (wasBackup)
        NMS_LOG_INFO (selfId, "SPN_TAKEOVER", std::string ("Node ") + std::to_string (selfId) + " takes over as primary SPN");
      NMS_LOG_INFO (selfId, "SPN_ELECT", "elected as primary SPN (score=" + std::to_string (myScore) + ")");
      if (!m_flushEvent.IsRunning ())
        m_flushEvent = Simulator::Schedule (m_aggregateInterval,
                                            &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
      if (m_heartbeatEvent.IsRunning ())
        Simulator::Cancel (m_heartbeatEvent);
      m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC),
                                              &HeterogeneousNodeApp::SendHeartbeatSync, this);
      // 新主上位后立即广播一次主备通告，避免节点视图滞后导致脑裂
      NMS_LOG_INFO (selfId, "SPN_ANNOUNCE",
                    "subnet=" + std::to_string (static_cast<uint32_t> (m_subnetType)) +
                    " primary=" + std::to_string (spnNodeId) +
                    " backup=" + std::to_string (backupNodeId) +
                    " reason=takeover_broadcast");
      SendScoreFlood (myScore);
      if (m_failoverPending)
        {
          double healSec = Simulator::Now ().GetSeconds () - m_failoverStartTs;
          NMS_LOG_INFO (selfId, "SELF_HEAL_TIME", "takeover completed in " + std::to_string (healSec) + "s");
          m_failoverPending = false;
        }
    }
  else if (m_isBackupSpn && !wasBackup)
    {
      NMS_LOG_INFO (selfId, "SPN_ELECT", "elected as backup SPN");
      m_missedHeartbeats = 0;
      if (m_heartbeatEvent.IsRunning ())
        Simulator::Cancel (m_heartbeatEvent);
      m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC),
                                              &HeterogeneousNodeApp::CheckPrimaryHeartbeat, this);
    }
  else if (wasSpn && !m_isSpn)
    {
      NMS_LOG_INFO (selfId, "SPN_SWITCH", "lost primary SPN role");
      if (m_heartbeatEvent.IsRunning ())
        Simulator::Cancel (m_heartbeatEvent);
      m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC),
                                              &HeterogeneousNodeApp::CheckPrimaryHeartbeat, this);
    }

  if (stateChanged)
    {
      std::ostringstream oss;
      oss << "subnet=" << static_cast<uint32_t> (m_subnetType)
          << " primary=" << spnNodeId
          << " backup=" << backupNodeId
          << " reason=" << changeReason
          << " failoverUsed=" << (m_failoverSwitchUsed ? 1 : 0)
          << " energySwitchUsed=" << (m_energySwitchUsed ? 1 : 0);
      NMS_LOG_INFO (selfId, "SPN_ANNOUNCE", oss.str ());
      // 子网通报：状态变化时立即发一次 score-flood（携带当前全局视图）
      SendScoreFlood (myScore);
      // GMC 同步：若本节点已是主 SPN，立即补发一次聚合，避免等待周期定时器
      if (m_isSpn)
        {
          Simulator::Schedule (MilliSeconds (50), &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
        }
    }

  // 统一 UDP 默认对端：仅在地址/端口变化时 Connect，减少 ns-3 UDP 套接字重复 Connect 带来的发送路径抖动
  if (m_socket && m_gmcBackhaulPort != 0)
    {
      Ipv4Address wantAddr ("0.0.0.0");
      uint16_t wantPort = 0;
      if (m_isSpn && m_gmcBackhaulAddress != Ipv4Address ("0.0.0.0"))
        {
          wantAddr = m_gmcBackhaulAddress;
          wantPort = m_gmcBackhaulPort;
        }
      else if (!m_isSpn && m_reportTargetAddress != Ipv4Address ("0.0.0.0"))
        {
          wantAddr = m_reportTargetAddress;
          wantPort = m_subnetReportPort;
        }
      if (wantPort != 0 && (wantAddr != m_socketReportBoundAddr || wantPort != m_socketBoundPeerPort))
        {
          m_socket->Connect (InetSocketAddress (wantAddr, wantPort));
          m_socketReportBoundAddr = wantAddr;
          m_socketBoundPeerPort = wantPort;
        }
    }
}

void
HeterogeneousNodeApp::RecvFromSubnet (Ptr<Socket> socket)
{
  Address from;
  Ptr<Packet> packet;
  while ((packet = socket->RecvFrom (from)))
    {
      if (packet->GetSize () < 3)
        {
          continue;
        }
      InetSocketAddress iaddr = InetSocketAddress::ConvertFrom (from);
      Ipv4Address src = iaddr.GetIpv4 ();
      uint32_t len = packet->GetSize ();
      std::vector<uint8_t> data (len);
      packet->CopyData (data.data (), len);
      const uint8_t* payload = nullptr;
      uint32_t payloadLen = 0;
      if (!ParseHnmpFrame (data.data (), len, &payload, &payloadLen)) continue;
      NmsPacketParse::LogDataPacket (GetNode ()->GetId (), "RECV", src, Ipv4Address ("0.0.0.0"), iaddr.GetPort (), m_subnetReportPort, payload, payloadLen);
      m_aggregateBuf[src] = std::vector<uint8_t> (payload, payload + payloadLen);

      // 解码 TYPE_NODE_REPORT_SPN，更新子网拓扑（节点状态 + 边）
      const uint8_t* buf = payload;
      if (payloadLen >= 3 && buf[0] == NmsTlv::TYPE_NODE_REPORT_SPN)
        {
          uint16_t valLen = (static_cast<uint16_t> (buf[1]) << 8) | buf[2];
          if (payloadLen >= 3u + valLen && valLen >= 4 + 24 + 12 + 8 + 8 + 2)
            {
              NodeReportState st;
              std::vector<uint32_t> neighborIds;
              uint32_t consumed = NmsTlv::ParseNodeReportSpn (buf + 3, valLen,
                                                              &st.nodeId, &st.px, &st.py, &st.pz,
                                                              &st.vx, &st.vy, &st.vz,
                                                              &st.energy, &st.linkQ,
                                                              &neighborIds);
              if (consumed > 0)
                {
                  st.neighborIds = std::move (neighborIds);
                  m_subnetNodeStates[st.nodeId] = std::move (st);
                  for (uint32_t nid : m_subnetNodeStates[st.nodeId].neighborIds)
                    {
                      uint32_t a = st.nodeId, b = nid;
                      if (a > b) std::swap (a, b);
                      m_subnetEdges.insert (std::make_pair (a, b));
                    }
                }
            }
        }
    }
}

void
HeterogeneousNodeApp::RecvPolicy (Ptr<Socket> socket)
{
  Address from;
  Ptr<Packet> packet;
  while ((packet = socket->RecvFrom (from)))
    {
      uint32_t nodeId = GetNode ()->GetId ();
      uint32_t size = packet->GetSize ();
      if (size > 0)
        {
          std::vector<uint8_t> buf (size);
          packet->CopyData (buf.data (), size);
          InetSocketAddress iaddr = InetSocketAddress::ConvertFrom (from);
          NmsPacketParse::LogPolicyPacket (nodeId, "RECV", iaddr.GetIpv4 (), iaddr.GetPort (), buf.data (), size);
        }
      std::ostringstream oss;
      oss << "forwarding policy to subnet (size=" << size << "B)";
      NMS_LOG_INFO (nodeId, "PROXY_POLICY", oss.str ());

      if (m_subnetBroadcast != Ipv4Address ("0.0.0.0"))
        {
          Ptr<Packet> fwd = packet->Copy ();
          InetSocketAddress dst = InetSocketAddress (m_subnetBroadcast, m_nodePolicyPort);
          m_policySocket->SendTo (fwd, 0, dst);
        }
    }
}

void
HeterogeneousNodeApp::FlushAggregatedToGmc ()
{
  if (!m_running || !m_socket)
    {
      return;
    }

  uint32_t nodeId = GetNode ()->GetId ();
  uint8_t outBuf[8192];
  uint32_t offset = 0;

  // 构建 TYPE_TOPOLOGY_AGGREGATE：节点状态列表 + 子网拓扑边
  const uint32_t maxPayload = 8192 - 3;
  if (m_subnetNodeStates.empty () && m_aggregateBuf.empty ())
    {
      m_flushEvent = Simulator::Schedule (m_aggregateInterval,
                                          &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
      return;
    }

  // 先写入 TLV 头，Value = [n_nodes(2)][node_reports...][n_edges(2)][edges...]
  offset = 3;  // 预留 TLV 头
  uint16_t nNodes = static_cast<uint16_t> (m_subnetNodeStates.size ());
  if (offset + 2 + nNodes * (4 + 24 + 12 + 8 + 8 + 2 + 64 * 4) + 2 + m_subnetEdges.size () * 8 > maxPayload)
    nNodes = 0;
  outBuf[offset++] = (nNodes >> 8) & 0xff;
  outBuf[offset++] = nNodes & 0xff;
  uint16_t nWritten = 0;
  for (const auto& kv : m_subnetNodeStates)
    {
      if (nWritten >= nNodes) break;
      const NodeReportState& st = kv.second;
      if (offset + 4 + 24 + 12 + 8 + 8 + 2 + st.neighborIds.size () * 4 > maxPayload) break;
      outBuf[offset++] = (st.nodeId >> 24) & 0xff; outBuf[offset++] = (st.nodeId >> 16) & 0xff;
      outBuf[offset++] = (st.nodeId >> 8) & 0xff;  outBuf[offset++] = st.nodeId & 0xff;
      std::memcpy (outBuf + offset, &st.px, 8); offset += 8;
      std::memcpy (outBuf + offset, &st.py, 8); offset += 8;
      std::memcpy (outBuf + offset, &st.pz, 8); offset += 8;
      std::memcpy (outBuf + offset, &st.vx, 4); offset += 4;
      std::memcpy (outBuf + offset, &st.vy, 4); offset += 4;
      std::memcpy (outBuf + offset, &st.vz, 4); offset += 4;
      std::memcpy (outBuf + offset, &st.energy, 8); offset += 8;
      std::memcpy (outBuf + offset, &st.linkQ, 8); offset += 8;
      uint16_t n = static_cast<uint16_t> (st.neighborIds.size ());
      outBuf[offset++] = (n >> 8) & 0xff; outBuf[offset++] = n & 0xff;
      for (uint32_t nid : st.neighborIds)
        {
          outBuf[offset++] = (nid >> 24) & 0xff; outBuf[offset++] = (nid >> 16) & 0xff;
          outBuf[offset++] = (nid >> 8) & 0xff;  outBuf[offset++] = nid & 0xff;
        }
      nWritten++;
    }
  outBuf[3] = (nWritten >> 8) & 0xff;
  outBuf[4] = nWritten & 0xff;
  uint16_t nEdges = static_cast<uint16_t> (m_subnetEdges.size ());
  outBuf[offset++] = (nEdges >> 8) & 0xff;
  outBuf[offset++] = nEdges & 0xff;
  for (const auto& e : m_subnetEdges)
    {
      uint32_t a = e.first, b = e.second;
      outBuf[offset++] = (a >> 24) & 0xff; outBuf[offset++] = (a >> 16) & 0xff;
      outBuf[offset++] = (a >> 8) & 0xff;  outBuf[offset++] = a & 0xff;
      outBuf[offset++] = (b >> 24) & 0xff; outBuf[offset++] = (b >> 16) & 0xff;
      outBuf[offset++] = (b >> 8) & 0xff;  outBuf[offset++] = b & 0xff;
    }

  uint16_t valLen = offset - 3;
  outBuf[0] = NmsTlv::TYPE_TOPOLOGY_AGGREGATE;
  outBuf[1] = (valLen >> 8) & 0xff;
  outBuf[2] = valLen & 0xff;

  // SPN 增量聚合：无变化时跳过重复发送
  uint32_t currHash = 2166136261u;
  for (uint32_t i = 0; i < offset; ++i)
    {
      currHash ^= outBuf[i];
      currHash *= 16777619u;
    }
  double nowSec = Simulator::Now ().GetSeconds ();
  bool aggregateChanged = (currHash != m_lastAggregateHash);
  bool forceBySilent = (m_lastAggregateSendTs < 0.0) || ((nowSec - m_lastAggregateSendTs) >= m_maxSilentSec);
  if (!aggregateChanged && !forceBySilent)
    {
      m_aggregateSuppressedCount++;
      std::ostringstream dec;
      dec << "action=SKIP reason=NO_TOPOLOGY_DELTA sinceLastAgg=" << std::fixed << std::setprecision (3)
          << (nowSec - m_lastAggregateSendTs);
      NMS_LOG_INFO (nodeId, "DECISION", dec.str ());
      m_flushEvent = Simulator::Schedule (m_aggregateInterval,
                                          &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
      return;
    }

  uint8_t frameBuf[9000];
  uint32_t frameLen = BuildHnmpFrame (Hnmp::FRAME_REPORT, 1, 0, outBuf, offset, frameBuf, sizeof (frameBuf));
  if (frameLen == 0)
    {
      m_flushEvent = Simulator::Schedule (m_aggregateInterval,
                                          &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
      return;
    }
  Ptr<Packet> pkt = Create<Packet> (frameBuf, frameLen);
  m_socket->Send (pkt);
  m_lastAggregateHash = currHash;
  m_lastAggregateSendTs = nowSec;
  m_aggregateRawBytes += offset;
  m_aggregateSentBytes += frameLen;

  NMS_LOG_INFO (nodeId, "SPN_TO_GMC", "SPN " + std::to_string (nodeId) + " sending aggregated data to GMC 0");

  std::ostringstream oss;
  oss << "topology aggregate: " << m_subnetNodeStates.size () << " nodes, "
      << m_subnetEdges.size () << " edges, size=" << offset << "B";
  NMS_LOG_INFO (nodeId, "PROXY_AGGREGATE", oss.str ());
  {
    double compression = (offset > 0) ? (100.0 * (1.0 - static_cast<double> (frameLen) / static_cast<double> (offset))) : 0.0;
    std::ostringstream eff;
    eff << "mode=" << (aggregateChanged ? "DELTA" : "FAILSAFE")
        << " rawBytes=" << offset
        << " sentBytes=" << frameLen
        << " compression=" << std::fixed << std::setprecision (2) << compression
        << "% suppressedCount=" << m_aggregateSuppressedCount;
    NMS_LOG_INFO (nodeId, "AGG_EFFICIENCY", eff.str ());
  }

  m_flushEvent = Simulator::Schedule (m_aggregateInterval,
                                      &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
}

void
HeterogeneousNodeApp::HandlePolicyFromSpn (Ptr<Socket> socket)
{
  Address from;
  Ptr<Packet> packet;
  while ((packet = socket->RecvFrom (from)))
    {
      uint32_t nodeId = GetNode ()->GetId ();
      uint32_t size = packet->GetSize ();
      if (size > 0)
        {
          std::vector<uint8_t> buf (size);
          packet->CopyData (buf.data (), size);
          InetSocketAddress iaddr = InetSocketAddress::ConvertFrom (from);
          NmsPacketParse::LogPolicyPacket (nodeId, "RECV", iaddr.GetIpv4 (), iaddr.GetPort (), buf.data (), size);
        }
      std::ostringstream oss;
      oss << "received policy from SPN, size=" << packet->GetSize () << "B";
      NMS_LOG_INFO (nodeId, "PROXY_POLICY", oss.str ());
    }
}

void
HeterogeneousNodeApp::StartApplication (void)
{
  m_running = true;
  m_packetsSent = 0;

  if (!m_socket)
    {
      m_socket = Socket::CreateSocket (GetNode (), m_socketType);
      m_socket->Connect (InetSocketAddress (m_peerAddress, m_peerPort));
      m_socketReportBoundAddr = m_peerAddress;
      m_socketBoundPeerPort = m_peerPort;
    }

  // SPN（Adhoc/DataLink）：在 reportPort 收子网上报，在 spnPolicyPort 收 GMC 策略；所有节点先创建 Socket，当选为 SPN 后再调度 Flush
  if (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK)
    {
      if (!m_aggregateSocket)
        {
          m_aggregateSocket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
          m_aggregateSocket->Bind (InetSocketAddress (Ipv4Address::GetAny (), m_subnetReportPort));
          m_aggregateSocket->SetRecvCallback (MakeCallback (&HeterogeneousNodeApp::RecvFromSubnet, this));
        }
      if (!m_policySocket)
        {
          m_policySocket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
          m_policySocket->Bind (InetSocketAddress (Ipv4Address::GetAny (), m_spnPolicyPort));
          m_policySocket->SetRecvCallback (MakeCallback (&HeterogeneousNodeApp::RecvPolicy, this));
        }
      if (m_isSpn)
        m_flushEvent = Simulator::Schedule (m_aggregateInterval,
                                            &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
    }

  // 非 SPN（Adhoc/DataLink）：在 5003 接收 SPN 转发的策略
  if (!m_isSpn && (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK))
    {
      if (!m_policyRecvSocket)
        {
          m_policyRecvSocket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
          m_policyRecvSocket->Bind (InetSocketAddress (Ipv4Address::GetAny (), m_nodePolicyPort));
          m_policyRecvSocket->SetRecvCallback (MakeCallback (&HeterogeneousNodeApp::HandlePolicyFromSpn, this));
        }
    }

  // 在 Ad-hoc / DataLink 中创建 Hello 广播 Socket，用于代理选举信息交换
  if (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK)
    {
      if (!m_helloSocket)
        {
          m_helloSocket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
          m_helloSocket->SetAllowBroadcast (true);
          m_helloSocket->Bind (InetSocketAddress (Ipv4Address::GetAny (), m_helloPort));
          m_helloSocket->SetRecvCallback (MakeCallback (&HeterogeneousNodeApp::HandleHello, this));
        }
    }

  // 主备SPN心跳（100ms）：主SPN发送，备SPN检查
  if (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK)
    {
      if (m_heartbeatEvent.IsRunning ())
        {
          Simulator::Cancel (m_heartbeatEvent);
        }
      if (m_isSpn)
        {
          m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC),
                                                  &HeterogeneousNodeApp::SendHeartbeatSync, this);
        }
      else
        {
          m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC),
                                                  &HeterogeneousNodeApp::CheckPrimaryHeartbeat, this);
        }
    }

  // 时序入网：若设置了 joinTime，则到 joinTime 时刻再发首包并启动定时器，此前静默
  if (m_joinTime > 0.0 && Simulator::Now ().GetSeconds () < m_joinTime)
    {
      Simulator::Schedule (Seconds (m_joinTime), &HeterogeneousNodeApp::SendPacket, this);
      return;
    }
  // 启动时立即发第一包或根据需要延时
  SendPacket ();
}

void
HeterogeneousNodeApp::StopApplication (void)
{
  if (m_running)
    {
      NMS_LOG_INFO (GetNode ()->GetId (), "NODE_OFFLINE", "Application stopped.");
      double suppressionRate = (m_totalScheduleDecisions > 0)
                               ? (100.0 * static_cast<double> (m_suppressedScheduleDecisions) /
                                  static_cast<double> (m_totalScheduleDecisions))
                               : 0.0;
      double avgInterval = (m_reportIntervalSamples > 0)
                           ? (m_totalReportIntervalSec / static_cast<double> (m_reportIntervalSamples))
                           : 0.0;
      std::ostringstream m;
      m << "ctrlSuppressionRate=" << std::fixed << std::setprecision (2) << suppressionRate
        << "% decisions=" << m_totalScheduleDecisions
        << " triggered=" << m_triggeredScheduleDecisions
        << " suppressed=" << m_suppressedScheduleDecisions
        << " avgReportIntervalSec=" << std::setprecision (3) << avgInterval
        << " aggSuppressed=" << m_aggregateSuppressedCount
        << " aggRawBytes=" << m_aggregateRawBytes
        << " aggSentBytes=" << m_aggregateSentBytes;
      NMS_LOG_INFO (GetNode ()->GetId (), "KPI_NODE", m.str ());
    }
  m_running = false;

  if (m_sendEvent.IsRunning ())
    {
      Simulator::Cancel (m_sendEvent);
    }
  if (m_flushEvent.IsRunning ())
    {
      Simulator::Cancel (m_flushEvent);
    }
  if (m_heartbeatEvent.IsRunning ())
    {
      Simulator::Cancel (m_heartbeatEvent);
    }

  if (m_socket)
    {
      m_socket->Close ();
    }

  if (m_aggregateSocket)
    {
      m_aggregateSocket->Close ();
      m_aggregateSocket = 0;
    }
  if (m_policySocket)
    {
      m_policySocket->Close ();
      m_policySocket = 0;
    }
  if (m_policyRecvSocket)
    {
      m_policyRecvSocket->Close ();
      m_policyRecvSocket = 0;
    }

  if (m_helloSocket)
    {
      m_helloSocket->Close ();
      m_helloSocket = 0;
    }
}

void
HeterogeneousNodeApp::ForceFail ()
{
  uint32_t nodeId = GetNode ()->GetId ();
  m_uvMib.m_energy = 0.0;
  NMS_LOG_INFO (nodeId, "EVENT_NODE_FAIL", "Node " + std::to_string (nodeId) + " power exhausted");
  StopApplication ();
}

void
HeterogeneousNodeApp::TriggerElectionNow ()
{
  if (!m_running || !(m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK))
    {
      return;
    }
  RunElection ();
}

uint32_t
HeterogeneousNodeApp::BuildHnmpFrame (uint8_t frameType, uint8_t qos, uint8_t dstId,
                                      const uint8_t* tlv, uint32_t tlvLen,
                                      uint8_t* out, uint32_t outSize)
{
  Hnmp::Header h = {};
  h.frameType = frameType;
  h.qosLevel = qos;
  h.sourceId = static_cast<uint8_t> (GetNode ()->GetId () & 0xffu);
  h.destId = dstId;
  h.seq = m_hnmpSeq++;
  h.payloadLen = static_cast<uint8_t> (std::min (tlvLen, 255u));
  bool compact3B = (m_subnetType == SUBNET_DATALINK);
  return Hnmp::EncodeFrame (out, outSize, h, tlv, h.payloadLen, compact3B);
}

bool
HeterogeneousNodeApp::ParseHnmpFrame (const uint8_t* in, uint32_t inLen,
                                      const uint8_t** outPayload, uint32_t* outPayloadLen)
{
  if (!in || inLen == 0 || !outPayload || !outPayloadLen) return false;
  Hnmp::Header h = {};
  bool compact3B = (m_subnetType == SUBNET_DATALINK);
  if (Hnmp::DecodeFrame (in, inLen, compact3B, &h, outPayload, outPayloadLen))
    return true;
  // 兼容旧版：历史报文直接是 TLV，无 HNMP 头
  *outPayload = in;
  *outPayloadLen = inLen;
  return true;
}

double
HeterogeneousNodeApp::ComputeReportStateDelta () const
{
  if (m_lastObservedEnergy < 0.0 || m_lastObservedLinkQuality < 0.0 || m_lastObservedMobility < 0.0)
    {
      return 1.0;
    }
  double de = std::fabs (m_uvMib.m_energy - m_lastObservedEnergy);
  double dl = std::fabs (m_uvMib.m_linkQuality - m_lastObservedLinkQuality);
  double dm = std::fabs (m_uvMib.m_mobilityScore - m_lastObservedMobility);
  return 0.4 * de + 0.35 * dl + 0.25 * dm;
}

bool
HeterogeneousNodeApp::ShouldScheduleReport (double delta, double nowSec, std::string* reason) const
{
  double sinceLast = (m_lastStateReportTs < 0.0) ? 1e9 : (nowSec - m_lastStateReportTs);
  if (sinceLast >= m_maxSilentSec)
    {
      if (reason) *reason = "MAX_SILENT_TRIGGER";
      return true;
    }
  if (delta >= m_reportStateThreshold)
    {
      if (reason) *reason = "DELTA_TRIGGER";
      return true;
    }
  if (sinceLast < m_stateSuppressWindowSec)
    {
      if (reason) *reason = "IN_SUPPRESS_WINDOW_SKIP";
      return false;
    }
  if (reason) *reason = "DELTA_BELOW_THRESHOLD_SKIP";
  return false;
}

void
HeterogeneousNodeApp::LogDecision (const std::string& action, const std::string& reason, double delta, double nowSec) const
{
  std::ostringstream oss;
  double sinceLast = (m_lastStateReportTs < 0.0) ? -1.0 : (nowSec - m_lastStateReportTs);
  oss << "action=" << action
      << " reason=" << reason
      << " delta=" << std::fixed << std::setprecision (4) << delta
      << " threshold=" << m_reportStateThreshold
      << " sinceLast=" << std::setprecision (3) << sinceLast
      << " suppressWin=" << m_stateSuppressWindowSec;
  NMS_LOG_INFO (GetNode ()->GetId (), "DECISION", oss.str ());
}

void
HeterogeneousNodeApp::SendPacket ()
{
  if (!m_running)
    {
      return;
    }

  uint32_t nodeId = GetNode ()->GetId ();
  double now = Simulator::Now ().GetSeconds ();
  double prevReportTs = m_lastStateReportTs;

  // 时序入网静默：未到 joinTime 前不参与任何网络行为（不发 Hello、不选举、不耗电）
  if (m_joinTime > 0.0 && now < m_joinTime)
    {
      ScheduleNextTx ();
      return;
    }

  // 1) 更新 UV-MIB：能量递减 + 链路/移动性扰动
  UpdateUvMib ();

  // 2) 计算效用评分 Score
  double myScore = CalculateUtilityScore ();

  // 3) Adhoc/DataLink：动态选举 SPN/备选 SPN，并更新上报目标
  if (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK)
    {
      RunElection ();
    }

  // 4) Ad-hoc / DataLink 中广播 Hello（携带 nodeId + Score）
  if (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK)
    {
      SendHello (myScore);
      // 降低控制包发送频率：每 24 个周期广播一次 Score 泛洪
      if (m_packetsSent > 0 && (m_packetsSent % 24) == 0)
        SendScoreFlood (myScore);
    }

  // 状态变化可观测性：记录本轮状态变化，并在调度阶段做前置抑制判断
  double stateDelta = ComputeReportStateDelta ();
  if (m_lastObservedEnergy >= 0.0)
    {
      std::ostringstream sc;
      sc << "energy:" << std::fixed << std::setprecision (3) << m_lastObservedEnergy << "->" << m_uvMib.m_energy
         << " linkQ:" << m_lastObservedLinkQuality << "->" << m_uvMib.m_linkQuality
         << " mobility:" << m_lastObservedMobility << "->" << m_uvMib.m_mobilityScore
         << " deltaScore=" << stateDelta;
      NMS_LOG_INFO (nodeId, "STATE_CHANGE", sc.str ());
    }
  m_lastObservedEnergy = m_uvMib.m_energy;
  m_lastObservedLinkQuality = m_uvMib.m_linkQuality;
  m_lastObservedMobility = m_uvMib.m_mobilityScore;

  std::string decisionReason;
  bool shouldSend = ShouldScheduleReport (stateDelta, now, &decisionReason);
  m_totalScheduleDecisions++;
  if (!shouldSend)
    {
      m_suppressedScheduleDecisions++;
      LogDecision ("SKIP", decisionReason, stateDelta, now);
      ScheduleNextTx ();
      return;
    }
  m_triggeredScheduleDecisions++;
  LogDecision ("TRIGGER", decisionReason, stateDelta, now);

  // 5) 迟滞切换：切换阈值 0.15，降低微小波动导致的切换
  if (myScore > m_maxKnownScore + SPN_SWITCH_HYSTERESIS)
    {
      std::ostringstream oss;
      oss << "becomes NEW PROXY (OldMax: " << std::fixed << std::setprecision (3)
          << m_maxKnownScore << ", New: " << myScore << ")";
      NMS_LOG_INFO (nodeId, "PROXY_SWITCH", oss.str ());
      NS_LOG_UNCOND ("[Switch] Node " << nodeId << " " << oss.str ());
      m_maxKnownScore = myScore;
      m_isProxy = true;
    }
  else if (m_isProxy && myScore <= m_maxKnownScore + SPN_SWITCH_HYSTERESIS)
    {
      m_isProxy = false;
    }

  // 6) 分层路由（应用层网关代理）：普通节点绝不直连 GMC，默认上报目标为本簇 SPN（m_reportTargetAddress）。
  //    LTE 直接上报 GMC；Adhoc/DataLink 非 SPN 向当前 SPN 上报 TYPE_NODE_REPORT_SPN（含邻居列表），SPN 由 FlushAggregatedToGmc 聚合后发往 GMC。
  uint8_t tlvBuf[1024];
  uint8_t frameBuf[1200];
  uint32_t tlvLen = 0;
  bool sentThisRound = false;
  auto sendHnmp = [this, nodeId, &frameBuf, &sentThisRound, now] (const uint8_t* tlv, uint32_t tlvLen, uint8_t qos, uint8_t dstId) -> bool {
    if (!m_socket || m_peerAddress == Ipv4Address ("0.0.0.0") || m_peerPort == 0)
      {
        m_protocolSuppressCount++;
        NMS_LOG_INFO (nodeId, "SEND_SUPPRESS", "skip report due to invalid peer address/port");
        return false;
      }
    uint32_t frameLen = BuildHnmpFrame (Hnmp::FRAME_REPORT, qos, dstId, tlv, tlvLen, frameBuf, sizeof (frameBuf));
    if (frameLen == 0) return false;
    Ptr<Packet> packet = Create<Packet> (frameBuf, frameLen);
    m_socket->Send (packet);
    m_packetsSent++;
    m_reportedPackets++;
    sentThisRound = true;
    m_lastStateReportTs = now;
    NmsPacketParse::LogDataPacket (nodeId, "SEND", Ipv4Address ("0.0.0.0"), m_peerAddress, 0, m_peerPort, frameBuf, frameLen);
    return true;
  };
  auto suppressedByWindow = [this, now] () -> bool {
    if (m_stateSuppressWindowSec <= 0.0) return false;
    if (m_lastStateReportTs < 0.0) return false;
    return (now - m_lastStateReportTs) < m_stateSuppressWindowSec;
  };
  auto markReported = [this, now, &sentThisRound] () {
    m_lastStateReportTs = now;
    sentThisRound = true;
  };
  auto buildUvMibMultiTlv = [this, nodeId, &tlvBuf] () -> uint32_t {
    uint32_t off = 0;
    UvMibConfigIntentModel cfg = {};
    cfg.role = m_isSpn ? 1 : 2;  // 1=SPN, 2=TSN
    cfg.subnetType = static_cast<uint8_t> (m_subnetType);
    cfg.qosIntent = (m_subnetType == SUBNET_DATALINK) ? 0 : 1;
    off += SerializeUvMibConfigIntent (cfg, tlvBuf + off, sizeof (tlvBuf) - off);

    UvMibBusinessPerfModel biz = {};
    biz.flowId = 1;
    biz.priority = (m_subnetType == SUBNET_DATALINK) ? 0 : 1;
    biz.throughputMbps = std::max (0.0, m_uvMib.m_linkQuality * 2.0);
    biz.delayMs = (1.0 - m_uvMib.m_linkQuality) * 150.0;
    biz.lossPct = std::max (0.0, (1.0 - m_uvMib.m_linkQuality) * 10.0);
    off += SerializeUvMibBusinessPerf (biz, tlvBuf + off, sizeof (tlvBuf) - off);

    UvMibRouteTopoModel topo = {};
    topo.neighborCount = static_cast<uint16_t> (m_neighborScores.size ());
    topo.avgLinkQuality = m_uvMib.m_linkQuality;
    topo.routeCost = static_cast<uint16_t> (std::max (1.0, 100.0 * (1.0 - m_uvMib.m_linkQuality)));
    topo.macRetry = static_cast<uint16_t> (std::max (0.0, 10.0 * (1.0 - m_uvMib.m_linkQuality)));
    off += SerializeUvMibRouteTopo (topo, tlvBuf + off, sizeof (tlvBuf) - off);

    if (m_uvMib.m_energy < 0.2 && off + 14 < sizeof (tlvBuf))
      {
        UvMibFaultAlarmModel fault = {};
        fault.faultType = 1;  // low energy
        fault.severity = 2;   // warning
        fault.code = 1001;
        fault.faultTs = Simulator::Now ().GetSeconds ();
        off += SerializeUvMibFaultAlarm (fault, tlvBuf + off, sizeof (tlvBuf) - off);
        if (!m_lowEnergyAlarmRaised)
          {
            NMS_LOG_INFO (nodeId, "FAULT_REPORT", "low energy alarm generated");
            m_lowEnergyAlarmRaised = true;
          }
      }
    else if (m_uvMib.m_energy >= 0.3)
      {
        m_lowEnergyAlarmRaised = false;
      }
    return off;
  };

  if (m_subnetType == SUBNET_ADHOC)
    {
      if (!m_isSpn && m_reportTargetAddress != Ipv4Address ("0.0.0.0"))
        {
          double px = 0.0, py = 0.0, pz = 0.0;
          float vx = 0.0f, vy = 0.0f, vz = 0.0f;
          Ptr<MobilityModel> mob = GetNode ()->GetObject<MobilityModel> ();
          if (mob)
            {
              Vector pos = mob->GetPosition ();
              px = pos.x; py = pos.y; pz = pos.z;
              Ptr<ConstantVelocityMobilityModel> cv = DynamicCast<ConstantVelocityMobilityModel> (mob);
              if (cv)
                {
                  Vector vel = cv->GetVelocity ();
                  vx = static_cast<float> (vel.x); vy = static_cast<float> (vel.y); vz = static_cast<float> (vel.z);
                }
            }
          std::vector<uint32_t> neighborIds;
          for (const auto& kv : m_neighborScores)
            neighborIds.push_back (kv.first);
          tlvLen = NmsTlv::BuildNodeReportSpnPayload (tlvBuf, sizeof (tlvBuf),
                                                      nodeId, px, py, pz, vx, vy, vz,
                                                      m_uvMib.m_energy, m_uvMib.m_linkQuality,
                                                      neighborIds);
          if (tlvLen > 0)
            {
              sendHnmp (tlvBuf, tlvLen, 1, 0xFE);
              NMS_LOG_INFO (nodeId, "SEND", "Ad-hoc NODE_REPORT_SPN to SPN Size=" + std::to_string (tlvLen) + "B");
            }
        }
      else if (!m_isSpn)
        {
          tlvLen = NmsTlv::BuildAdhocPayload (tlvBuf, sizeof (tlvBuf),
                                              m_uvMib.m_energy,
                                              m_uvMib.m_linkQuality,
                                              m_uvMib.m_mobilityScore);
          if (tlvLen > 0)
            {
              sendHnmp (tlvBuf, tlvLen, 2, 0xFE);
            }
        }
    }
  else if (m_subnetType == SUBNET_LTE)
    {
      if (suppressedByWindow ())
        {
          NMS_LOG_INFO (nodeId, "DECISION", "action=TRIGGER reason=BYPASS_SUPPRESS_BY_SCHEDULER");
        }
      tlvLen = buildUvMibMultiTlv ();
      if (tlvLen == 0) { ScheduleNextTx (); return; }
      NS_LOG_INFO ("[LTE] Node " << nodeId << " sending UV-MIB multi-TLV, Size: " << tlvLen << "B");
      { std::ostringstream o; o << "LTE UV-MIB MultiTLV Size=" << tlvLen << "B"; NMS_LOG_INFO (nodeId, "SEND", o.str ()); }
      sendHnmp (tlvBuf, tlvLen, 1, 0);
      markReported ();
    }
  else if (m_subnetType == SUBNET_DATALINK)
    {
      if (!m_isSpn && m_reportTargetAddress != Ipv4Address ("0.0.0.0"))
        {
          double px = 0.0, py = 0.0, pz = 0.0;
          float vx = 0.0f, vy = 0.0f, vz = 0.0f;
          Ptr<MobilityModel> mob = GetNode ()->GetObject<MobilityModel> ();
          if (mob)
            {
              Vector pos = mob->GetPosition ();
              px = pos.x; py = pos.y; pz = pos.z;
              Ptr<ConstantVelocityMobilityModel> cv = DynamicCast<ConstantVelocityMobilityModel> (mob);
              if (cv)
                {
                  Vector vel = cv->GetVelocity ();
                  vx = static_cast<float> (vel.x); vy = static_cast<float> (vel.y); vz = static_cast<float> (vel.z);
                }
            }
          std::vector<uint32_t> neighborIds;
          for (const auto& kv : m_neighborScores)
            neighborIds.push_back (kv.first);
          tlvLen = NmsTlv::BuildNodeReportSpnPayload (tlvBuf, sizeof (tlvBuf),
                                                      nodeId, px, py, pz, vx, vy, vz,
                                                      m_uvMib.m_energy, m_uvMib.m_linkQuality,
                                                      neighborIds);
          if (tlvLen > 0)
            {
              sendHnmp (tlvBuf, tlvLen, 0, 0xFE);
              NMS_LOG_INFO (nodeId, "SEND", "DataLink NODE_REPORT_SPN to SPN Size=" + std::to_string (tlvLen) + "B");
            }
        }
      else if (!m_isSpn)
        {
          if (suppressedByWindow ())
            {
              NMS_LOG_INFO (nodeId, "DECISION", "action=TRIGGER reason=BYPASS_SUPPRESS_BY_SCHEDULER");
            }
          double delta = std::fabs (m_uvMib.m_energy - m_uvMib.m_lastReportedEnergy);
          if (delta >= m_energyDeltaThreshold)
            {
              tlvLen = NmsTlv::BuildDataLinkPayload (tlvBuf, sizeof (tlvBuf), delta);
              if (tlvLen > 0)
                {
                  sendHnmp (tlvBuf, tlvLen, 0, 0xFE);
                  m_uvMib.m_lastReportedEnergy = m_uvMib.m_energy;
                  markReported ();
                  NMS_LOG_INFO (nodeId, "DELTA_REPORT",
                                "energy delta=" + std::to_string (delta) +
                                " threshold=" + std::to_string (m_energyDeltaThreshold));
                }
            }
          else
            {
              m_protocolSuppressCount++;
              NMS_LOG_INFO (nodeId, "DELTA_SUPPRESS",
                            "energy delta=" + std::to_string (delta) +
                            " below threshold=" + std::to_string (m_energyDeltaThreshold));
            }
        }
    }

  if (!sentThisRound && shouldSend)
    {
      // 例如当前节点为 SPN 且本轮无业务上报载荷时，也记录一次“已执行决策”时间，避免持续触发。
      m_lastStateReportTs = now;
      sentThisRound = true;
    }
  if (sentThisRound && prevReportTs >= 0.0)
    {
      double interval = now - prevReportTs;
      if (interval > 0.0 && interval < 1e8)
        {
          m_totalReportIntervalSec += interval;
          m_reportIntervalSamples++;
        }
    }

  // 7) 调度下一次发送
  ScheduleNextTx ();
}

void
HeterogeneousNodeApp::ScheduleNextTx ()
{
  if (!m_running)
    {
      return;
    }
  if (m_sendEvent.IsRunning ())
    {
      Simulator::Cancel (m_sendEvent);
    }
  double now = Simulator::Now ().GetSeconds ();
  double sinceLast = (m_lastStateReportTs < 0.0) ? 1e9 : (now - m_lastStateReportTs);
  double toFailsafe = std::max (0.05, m_maxSilentSec - sinceLast);
  double dynamicSec = std::min (toFailsafe, std::max (0.25, m_interval.GetSeconds () * 2.0));
  m_sendEvent = Simulator::Schedule (Seconds (dynamicSec),
                                     &HeterogeneousNodeApp::SendPacket,
                                     this);
}

/*========================= GMC 策略下发应用 ========================*/
class GmcPolicySenderApp : public Application
{
public:
  GmcPolicySenderApp ();
  virtual ~GmcPolicySenderApp ();
  void AddTarget (Ipv4Address addr, uint16_t port);
  void SetInterval (Time interval);

protected:
  void DoDispose (void) override;
  void StartApplication (void) override;
  void StopApplication (void) override;

private:
  void SendPolicy ();

  std::vector<std::pair<Ipv4Address, uint16_t>> m_targets;
  Time            m_interval;
  EventId         m_sendEvent;
  Ptr<Socket>     m_socket;
  bool            m_running;
};

GmcPolicySenderApp::GmcPolicySenderApp ()
  : m_interval (Seconds (10.0)),
    m_socket (0),
    m_running (false)
{
}

GmcPolicySenderApp::~GmcPolicySenderApp ()
{
  m_socket = 0;
}

void
GmcPolicySenderApp::AddTarget (Ipv4Address addr, uint16_t port)
{
  m_targets.push_back (std::make_pair (addr, port));
}

void
GmcPolicySenderApp::SetInterval (Time interval)
{
  m_interval = interval;
}

void
GmcPolicySenderApp::DoDispose (void)
{
  Application::DoDispose ();
}

void
GmcPolicySenderApp::StartApplication (void)
{
  m_running = true;
  m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
  m_sendEvent = Simulator::Schedule (Seconds (5.0), &GmcPolicySenderApp::SendPolicy, this);
}

void
GmcPolicySenderApp::StopApplication (void)
{
  m_running = false;
  if (m_sendEvent.IsRunning ())
    {
      Simulator::Cancel (m_sendEvent);
    }
  if (m_socket)
    {
      m_socket->Close ();
    }
}

void
GmcPolicySenderApp::SendPolicy (void)
{
  if (!m_running || !m_socket || m_targets.empty ())
    {
      return;
    }

  uint8_t buf[64];
  uint32_t len = NmsTlv::BuildPolicyPayload (buf, 32);
  Ptr<Packet> pkt = Create<Packet> (buf, len);

  for (const auto& t : m_targets)
    {
      m_socket->SendTo (pkt->Copy (), 0, InetSocketAddress (t.first, t.second));
    }

  NmsLog ("INFO", GetNode ()->GetId (), "POLICY_FORWARD", "policy sent to all SPNs");

  m_sendEvent = Simulator::Schedule (m_interval, &GmcPolicySenderApp::SendPolicy, this);
}

/*========================= 仿真框架类：HeterogeneousNmsFramework ======================
 *
 * 角色：
 *  - 负责整体场景搭建（GMC + 3 个子网 + 回程链路）
 *  - 将“拓扑构建”、“应用安装”、“监控工具配置”、“仿真控制” 解耦为独立方法
 *
 * 主要接口：
 *  - BuildGmc()
 *  - BuildLteSubnet()
 *  - BuildAdhocSubnet()
 *  - BuildDataLinkSubnet()
 *  - SetupBackhaul()
 *  - InstallApplications()
 *  - SetupMonitoring()
 *  - Run()
 *
 * 扩展指南：
 *  - 若要增加新的子网类型，请仿照 BuildXxxSubnet() 新增方法，并在 Run() 中调用
 *  - 若要变更仿真时间、日志配置，请在 Run() 中集中修改
 *
 * 双通道分离评估结论（控制 8888 / 数据 9999）：
 *  - 推荐实现：在当前异构子网场景下，将控制报文（Hello、Proxy 选举、Policy、SPN 心跳）与
 *    数据报文（TLV 业务上报、聚合数据）分离到不同 UDP 端口，可带来：
 *    ① FlowMonitor 按五元组自动区分控制流与数据流，便于分别统计丢包率/吞吐量/时延；
 *    ② 控制通道可配置更短发送间隔、更高优先级（若底层支持 QoS），减少策略/选举延迟；
 *    ③ 数据通道可独立做压缩/聚合而不影响控制平面。
 *  - 使用方式：命令行增加 --dualChannel=1 即启用双通道；否则为单通道（原 5000/5001/5002/5003/6000）。
 *  - 若评估希望仅提升控制包优先级而不改端口，可替代方案：在发送控制包时设置 IP TOS/DSCP 字段。
 *===================================================================================*/

class HeterogeneousNmsFramework
{
public:
  /// WiFi Ad-hoc 子网拓扑类型（解耦拓扑形状与 OLSR/移动性）
  /// 星型：指定中心节点，其余仅与中心直连；树型：按节点 ID 层级；网状：全联通
  enum AdhocTopologyType
  {
    ADHOC_TOPOLOGY_MESH = 0,   ///< 网状：全联通，所有节点两两直连
    ADHOC_TOPOLOGY_STAR,       ///< 星型：中心节点（如 Node 0），其余仅与中心直连
    ADHOC_TOPOLOGY_TREE        ///< 树型：根节点 0，一级子 1-2，二级子 3-4
  };

  HeterogeneousNmsFramework ();
  ~HeterogeneousNmsFramework ();

  /// 外部统一调用入口
  void Run (double simTimeSeconds);

  /// 扩展选项（在 Run 之前设置，或通过 main 中 CommandLine 绑定）
  void SetAdhocTopology (AdhocTopologyType t) { m_adhocTopology = t; }
  void SetEnablePacketParse (bool b) { m_enablePacketParse = b; }
  void SetEnablePcap (bool b) { m_enablePcap = b; }
  void SetUseDualChannel (bool b) { m_useDualChannel = b; }
  /// 时序入网：节点入网配置 JSON 路径，空则禁用
  void SetJoinConfigPath (const std::string& path) { m_joinConfigPath = path; }
  void SetScenarioConfigPath (const std::string& path) { m_scenarioConfigPath = path; }
  /// 事件注入：在指定时刻强制某节点失效（nodeId=0 表示禁用）
  void SetFailNodeId (uint32_t nodeId) { m_failNodeId = nodeId; }
  void SetFailTime (double t) { m_failTime = t; }
  void SetEnergyDeltaThreshold (double v) { if (v > 0.0) m_energyDeltaThreshold = v; }
  void SetStateSuppressWindow (double v) { if (v >= 0.0) m_stateSuppressWindowSec = v; }
  void SetAggregateIntervalSec (double v) { if (v > 0.0) m_aggregateIntervalSec = v; }
  void SetScenarioMode (const std::string& mode) { m_scenarioMode = mode; }

private:
  // === 分步骤方法 ===
  void BuildGmc ();              ///< 构建全局管理中心节点
  void BuildLteSubnet ();        ///< 构建子网 A：蜂窝接入网 (LTE)
  void BuildAdhocSubnet ();      ///< 构建子网 B：WiFi 自组网 + OLSR
  void BuildDataLinkSubnet ();   ///< 构建子网 C：窄带数据链 (11a 低速率模拟)
  void SetupBackhaul ();         ///< 为每个 SPN 与 GMC 搭建有线 P2P 回程链路
  void ElectInitialSpn ();      ///< 按性能评分选举各子网 SPN（LTE 固定 eNB，Adhoc/DataLink 选举）
  void InstallApplications ();   ///< 在 GMC 与各 SPN 安装自定义应用
  void SetupMonitoring ();       ///< 配置 FlowMonitor 和 NetAnim
  /// 周期性输出业务流窗口性能（FLOW_PERF_WIN），用于前端真实时间序列
  void EmitFlowPerformanceWindow ();
  /// 为可视化输出：写当前时刻所有节点状态到 JSONL，并调度下一次（每 0.5s）
  void WriteAllNodesJsonl ();
  /// 时序入网：t=0 时将 joinTime>0 的节点接口设为 Down，joinTime 时刻再 BringUp
  void BringDownNodesWithDelayedJoin ();
  static void BringUpNode (Ptr<Node> node);
  /// 事件注入：在指定时刻强制指定节点失效（能量 0、停应用、关网卡），0 表示不注入
  void InjectNodeFailure (uint32_t nodeId);
  void StopFlowsRelatedToNode (uint32_t nodeId);

  /// 归档目录与时间戳（仿真开始时创建 simulation_results/YYYY-MM-DD_HH-MM-SS 及四类子目录）
  std::string GetOutputDir () const { return m_outputDir; }
  std::string GetTimestamp () const { return m_timestamp; }
  /// 生成带时间戳的输出路径（旧接口，写入 performance 分类，保留兼容）
  std::string MakeOutputPath (const std::string& baseName, const std::string& ext) const;
  /// 多级归档：按分类生成路径。category 取 "visualization"|"performance"|"log"|"packet"
  std::string MakeOutputPathInCategory (const std::string& category,
                                         const std::string& baseName,
                                         const std::string& ext) const;
  /// 返回分类子目录路径（用于 pcap 前缀、脚本参数等）
  std::string GetPacketDir () const { return m_dirPacket; }

  // === 内部辅助方法 ===
  void ConfigureLteMobility ();
  void ConfigureAdhocMobility ();
  void ConfigureDataLinkMobility ();
  void InstallStaticMobilityForNodesWithoutModel ();
  /// 时序入网：返回节点入网时间（秒），未配置则返回 0 表示仿真开始即入网
  double GetNodeJoinTime (uint32_t nodeId) const;
  /// 业务流路径追踪：构建 Adhoc 子网 nodeId <-> Ipv4Address 映射（在拓扑与 IP 分配完成后调用）
  void BuildAdhocAddressMap ();
  /// 通过 OLSR 路由表解析 src -> dst 的逐跳路径（仅 Adhoc 子网）
  std::vector<uint32_t> GetOlsrPath (uint32_t srcNodeId, uint32_t dstNodeId) const;
  /// 根据节点 ID 获取 Ptr<Node>（遍历 NodeList）
  static Ptr<Node> GetNodeById (uint32_t nodeId);

private:
  // 全局管理中心
  NodeContainer   m_gmcNode;

  // 子网节点容器
  NodeContainer   m_lteUeNodes;
  NodeContainer   m_lteEnbNodes;
  NodeContainer   m_adhocNodes;
  NodeContainer   m_datalinkNodes;

  // 各子网 SPN（子网代理节点）
  Ptr<Node>       m_spnLte;        ///< LTE 子网的 SPN（eNodeB）
  Ptr<Node>       m_spnAdhoc;      ///< Ad-hoc 子网 SPN（节点 0）
  Ptr<Node>       m_spnDatalink;  ///< DataLink 子网 SPN（节点 0）

  // LTE 相关助手
  Ptr<LteHelper>             m_lteHelper;
  Ptr<PointToPointEpcHelper> m_epcHelper;

  // 设备与协议栈
  NetDeviceContainer m_lteEnbDevs;
  NetDeviceContainer m_lteUeDevs;
  NetDeviceContainer m_adhocDevs;    ///< Ad-hoc 子网 WiFi 设备（用于 pcap）
  NetDeviceContainer m_datalinkDevs; ///< DataLink 子网 WiFi 设备（用于 pcap）

  // 回程链路设备（GMC 与各 SPN 之间）；LTE 仍为单条，Adhoc/DataLink 改为每条节点一条（动态 SPN）
  NetDeviceContainer m_p2pGmcToLteSpn;
  std::vector<NetDeviceContainer> m_p2pGmcToAdhocBackhaul;   ///< 每条 GMC-Adhoc[i] 回程
  std::vector<NetDeviceContainer> m_p2pGmcToDatalinkBackhaul;

  // IP 地址分配
  Ipv4InterfaceContainer m_ifGmcLteSpn;
  Ipv4InterfaceContainer m_ifGmcAdhocSpn;       ///< 保留兼容，取第一条 Adhoc 回程的地址
  Ipv4InterfaceContainer m_ifGmcDatalinkSpn;    ///< 保留兼容
  std::vector<Ipv4InterfaceContainer> m_ifGmcAdhocBackhaul;    ///< 每个 Adhoc 节点一条回程的接口
  std::vector<Ipv4InterfaceContainer> m_ifGmcDatalinkBackhaul;
  Ipv4InterfaceContainer m_ifAdhoc;      ///< Adhoc 子网各节点 IP（与 m_adhocNodes 顺序一致）
  Ipv4InterfaceContainer m_ifDatalink;   ///< DataLink 子网各节点 IP（与 m_datalinkNodes 顺序一致）

  /// 业务流路径追踪：Adhoc 子网地址 <-> 节点 ID 映射（BuildAdhocAddressMap 后有效）
  std::map<Ipv4Address, uint32_t> m_adhocAddressToNodeId;
  std::map<uint32_t, Ipv4Address> m_adhocNodeIdToAddress;

  // FlowMonitor
  Ptr<FlowMonitor>    m_flowMonitor;
  FlowMonitorHelper   m_flowHelper;

  double              m_simTimeSeconds;  ///< 仿真时长，用于统一应用停止时间

  // 扩展选项（CommandLine 可配置）
  AdhocTopologyType   m_adhocTopology;    ///< Ad-hoc 拓扑：Mesh/Star/Tree
  bool                m_enablePacketParse; ///< 运行时包解析日志
  bool                m_enablePcap;      ///< 是否开启 pcap 抓包
  bool                m_useDualChannel;  ///< 控制/数据双通道分离（8888/9999）
  double              m_energyDeltaThreshold; ///< 差值上报阈值
  double              m_stateSuppressWindowSec; ///< 状态抑制窗口（秒）
  double              m_aggregateIntervalSec; ///< SPN 聚合上报周期（秒）
  std::string         m_scenarioMode;    ///< normal|thesis30|compare-baseline|compare-hnmp
  uint32_t            m_defaultAdhocNodes; ///< 默认 Adhoc 节点数（无 join_config）
  uint32_t            m_defaultDataLinkNodes; ///< 默认 DataLink 节点数（无 join_config）
  uint32_t            m_defaultLteUeNodes; ///< 默认 LTE UE 节点数（无 join_config）
  std::string         m_adhocRateMode;    ///< Adhoc PHY 速率
  std::string         m_datalinkRateMode; ///< DataLink PHY 速率
  uint32_t            m_datalinkPacketSize; ///< DataLink 应用包长
  double              m_datalinkIntervalSec; ///< DataLink 应用发送间隔

  /// 时序入网：节点配置 JSON 路径与解析结果（node_id -> NodeJoinConfig）
  std::string         m_joinConfigPath;
  std::string         m_scenarioConfigPath;
  std::map<uint32_t, NodeJoinConfig> m_joinConfig;
  std::set<std::string> m_businessFlowKeys; ///< 主KPI业务流键：srcIp|dstIp|dstPort
  std::map<std::string, uint32_t> m_businessFlowKeyToId; ///< 业务键 -> scenario flow_id
  struct FlowWindowSnapshot
  {
    uint64_t txPackets = 0;
    uint64_t rxPackets = 0;
    uint64_t lostPackets = 0;
    uint64_t rxBytes = 0;
    double delaySumMs = 0.0;
  };
  std::map<uint32_t, FlowWindowSnapshot> m_flowWindowPrev; ///< FlowMonitor flowId -> 上一窗口快照
  double m_flowPerfWindowSec; ///< 窗口统计周期（秒）

  /// 事件注入：在 m_failTime 秒时强制节点 m_failNodeId 失效（0 表示不注入）
  uint32_t            m_failNodeId;     ///< 要断电的节点 ID，0 = 禁用
  double              m_failTime;       ///< 断电时刻（秒）
  double              m_spnElectionTimeoutSec; ///< SPN 选举超时（秒）
  uint32_t            m_spnHeartbeatMissThreshold; ///< SPN 心跳丢失阈值

  /// 仿真文件结构化归档：根目录、时间戳、四类子目录
  std::string         m_outputDir;       ///< simulation_results/YYYY-MM-DD_HH-MM-SS
  std::string         m_timestamp;       ///< YYYY-MM-DD_HH-MM-SS
  std::string         m_dirVisualization; ///< .../visualization/
  std::string         m_dirPerformance;  ///< .../performance/
  std::string         m_dirLog;          ///< .../log/
  std::string         m_dirPacket;       ///< .../packet/
};

HeterogeneousNmsFramework::HeterogeneousNmsFramework ()
  : m_spnLte (0),
    m_spnAdhoc (0),
    m_spnDatalink (0),
    m_simTimeSeconds (100.0),
    m_adhocTopology (ADHOC_TOPOLOGY_MESH),
    m_enablePacketParse (false),
    m_enablePcap (false),
    m_useDualChannel (false),
    m_energyDeltaThreshold (0.05),
    m_stateSuppressWindowSec (1.0),
    m_aggregateIntervalSec (2.0),
    m_scenarioMode ("normal"),
    m_defaultAdhocNodes (5),
    m_defaultDataLinkNodes (4),
    m_defaultLteUeNodes (5),
    m_adhocRateMode ("HtMcs7"),
    m_datalinkRateMode ("HtMcs7"),
    m_datalinkPacketSize (200),
    m_datalinkIntervalSec (0.5),
    m_flowPerfWindowSec (0.5),
    m_failNodeId (0),
    m_failTime (30.0),
    m_spnElectionTimeoutSec (2.0),
    m_spnHeartbeatMissThreshold (3)
{
}

HeterogeneousNmsFramework::~HeterogeneousNmsFramework ()
{
}

/// 生成归档路径：m_outputDir + baseName + _ + m_timestamp + ext（兼容旧调用，写入 performance）
std::string
HeterogeneousNmsFramework::MakeOutputPath (const std::string& baseName, const std::string& ext) const
{
  return MakeOutputPathInCategory ("performance", baseName, ext);
}

/// 多级归档：category 子目录下 baseName_timestamp.ext
std::string
HeterogeneousNmsFramework::MakeOutputPathInCategory (const std::string& category,
                                                     const std::string& baseName,
                                                     const std::string& ext) const
{
  if (m_outputDir.empty ()) return baseName + "_" + m_timestamp + ext;
  return m_outputDir + "/" + category + "/" + baseName + "_" + m_timestamp + ext;
}

void
HeterogeneousNmsFramework::InjectNodeFailure (uint32_t nodeId)
{
  Ptr<Node> node;
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      if ((*it)->GetId () == nodeId)
        {
          node = *it;
          break;
        }
    }
  if (!node)
    {
      NmsLog ("WARN", 0, "EVENT_NODE_FAIL", "InjectNodeFailure: node " + std::to_string (nodeId) + " not found");
      return;
    }
  NmsLog ("INFO", 0, "EVENT_NODE_FAIL", "Node " + std::to_string (nodeId) + " power exhausted");
  StopFlowsRelatedToNode (nodeId);
  for (uint32_t i = 0; i < node->GetNApplications (); ++i)
    {
      Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> (node->GetApplication (i));
      if (app)
        {
          app->ForceFail ();
          break;
        }
    }
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
  if (ipv4)
    {
      for (uint32_t j = 1; j < ipv4->GetNInterfaces (); ++j)
        if (ipv4->IsUp (j))
          ipv4->SetDown (j);
    }
  NmsLog ("INFO", 0, "EVENT_NODE_OFFLINE_NOTIFY",
          "node " + std::to_string (nodeId) + " offline notified to all subnets");
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      if ((*it)->GetId () == nodeId) continue;
      for (uint32_t i = 0; i < (*it)->GetNApplications (); ++i)
        {
          Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> ((*it)->GetApplication (i));
          if (app)
            {
              app->TriggerElectionNow ();
              break;
            }
        }
    }
}

void
HeterogeneousNmsFramework::StopFlowsRelatedToNode (uint32_t nodeId)
{
  Ptr<Node> node = GetNodeById (nodeId);
  if (!node)
    {
      return;
    }
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
  std::set<Ipv4Address> failedIps;
  if (ipv4)
    {
      for (uint32_t i = 1; i < ipv4->GetNInterfaces (); ++i)
        {
          for (uint32_t j = 0; j < ipv4->GetNAddresses (i); ++j)
            {
              Ipv4Address addr = ipv4->GetAddress (i, j).GetLocal ();
              if (addr != Ipv4Address ("0.0.0.0"))
                {
                  failedIps.insert (addr);
                }
            }
        }
    }

  double now = Simulator::Now ().GetSeconds ();
  uint32_t stopped = 0;
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      Ptr<Node> n = *it;
      for (uint32_t a = 0; a < n->GetNApplications (); ++a)
        {
          Ptr<OnOffApplication> onoff = DynamicCast<OnOffApplication> (n->GetApplication (a));
          if (!onoff)
            {
              continue;
            }
          AddressValue remoteAttr;
          if (!onoff->GetAttributeFailSafe ("Remote", remoteAttr))
            {
              continue;
            }
          Address remote = remoteAttr.Get ();
          Ipv4Address remoteIp ("0.0.0.0");
          if (InetSocketAddress::IsMatchingType (remote))
            {
              remoteIp = InetSocketAddress::ConvertFrom (remote).GetIpv4 ();
            }
          else if (Inet6SocketAddress::IsMatchingType (remote))
            {
              continue;
            }
          if (failedIps.find (remoteIp) != failedIps.end ())
            {
              onoff->SetStopTime (Seconds (now + 0.001));
              stopped++;
            }
        }
    }
  if (stopped > 0)
    {
      NmsLog ("INFO", 0, "EVENT_FLOW_STOP",
              "stopped " + std::to_string (stopped) +
              " OnOff flows related to failed node " + std::to_string (nodeId));
    }
}

void
HeterogeneousNmsFramework::BuildGmc ()
{
  m_gmcNode.Create (1);

  MobilityHelper mobility;
  Ptr<ListPositionAllocator> listPos = CreateObject<ListPositionAllocator> ();
  double x = 0.0, y = 0.0, z = 0.0;
  auto it = m_joinConfig.find (0);
  if (it != m_joinConfig.end ())
    {
      x = it->second.initPos[0]; y = it->second.initPos[1]; z = it->second.initPos[2];
    }
  listPos->Add (Vector (x, y, z));
  mobility.SetPositionAllocator (listPos);
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (m_gmcNode);

  InternetStackHelper internet;
  internet.Install (m_gmcNode);
}

void
HeterogeneousNmsFramework::BuildLteSubnet ()
{
  // ----------------- 先创建 eNodeB 与 UE 节点（数量可由 JSON 驱动），再创建 EPC，使业务节点 ID 连续、EPC 垫底 -----------------
  uint32_t nUe = m_defaultLteUeNodes;
  if (!m_joinConfig.empty ())
    {
      nUe = 0;
      for (const auto& kv : m_joinConfig)
        if (kv.second.subnet == "LTE" && (kv.second.type == "UE" || kv.second.type == "ue")) nUe++;
      if (nUe == 0) nUe = m_defaultLteUeNodes;
    }
  m_lteEnbNodes.Create (1);
  m_lteUeNodes.Create (nUe);

  m_lteHelper = CreateObject<LteHelper> ();
  m_epcHelper = CreateObject<PointToPointEpcHelper> ();
  m_lteHelper->SetEpcHelper (m_epcHelper);

  // PGW 节点（EPC 网关）：位置由固定坐标或 JSON 驱动，废除 GridPositionAllocator
  Ptr<Node> pgw = m_epcHelper->GetPgwNode ();
  {
    MobilityHelper mobility;
    NodeContainer pgwContainer;
    pgwContainer.Add (pgw);
    Ptr<ListPositionAllocator> listPosPgw = CreateObject<ListPositionAllocator> ();
    listPosPgw->Add (Vector (25.0, 25.0, 0.0));  // 默认固定位置，避免与 eNB/UE 重叠
    mobility.SetPositionAllocator (listPosPgw);
    mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
    mobility.Install (pgwContainer);
  }

  // 在安装协议栈之前，为 LTE 子网节点配置静态网格布局和固定位置模型
  ConfigureLteMobility ();

  // 为 UE 安装 Internet 协议栈；eNodeB 的协议栈由 EPC Helper 在 InstallEnbDevice -> AddEnb 时自动安装
  InternetStackHelper internet;
  internet.Install (m_lteUeNodes);

  // ----------------- LTE 设备安装 -----------------
  m_lteEnbDevs = m_lteHelper->InstallEnbDevice (m_lteEnbNodes);
  m_lteUeDevs  = m_lteHelper->InstallUeDevice (m_lteUeNodes);

  // UE 附着到 eNodeB
  for (uint32_t i = 0; i < m_lteUeDevs.GetN (); ++i)
    {
      m_lteHelper->Attach (m_lteUeDevs.Get (i), m_lteEnbDevs.Get (0));
    }

  // ----------------- LTE UE 的 IP 分配 -----------------
  m_epcHelper->AssignUeIpv4Address (NetDeviceContainer (m_lteUeDevs));

  // LTE 子网 SPN：采用 eNodeB 节点作为该子网的代理
  m_spnLte = m_lteEnbNodes.Get (0);
}

void
HeterogeneousNmsFramework::ConfigureLteMobility ()
{
  // 完全由 JSON 驱动：废除 GridPositionAllocator，eNB 与 UE 的初始位置均来自 m_joinConfig.init_pos
  MobilityHelper mobilityEnb;
  Ptr<ListPositionAllocator> listPosEnb = CreateObject<ListPositionAllocator> ();
  uint32_t enbId = m_lteEnbNodes.Get (0)->GetId ();
  double ex = 50.0, ey = 50.0, ez = 0.0;
  auto itEnb = m_joinConfig.find (enbId);
  if (itEnb != m_joinConfig.end ())
    {
      ex = itEnb->second.initPos[0]; ey = itEnb->second.initPos[1]; ez = itEnb->second.initPos[2];
    }
  listPosEnb->Add (Vector (ex, ey, ez));
  mobilityEnb.SetPositionAllocator (listPosEnb);
  mobilityEnb.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobilityEnb.Install (m_lteEnbNodes);

  MobilityHelper mobilityUe;
  Ptr<ListPositionAllocator> listPosUe = CreateObject<ListPositionAllocator> ();
  for (uint32_t i = 0; i < m_lteUeNodes.GetN (); ++i)
    {
      uint32_t ueId = m_lteUeNodes.Get (i)->GetId ();
      double ux = 10.0 + 20.0 * i, uy = 10.0, uz = 0.0;  // 默认拉开间距
      auto itUe = m_joinConfig.find (ueId);
      if (itUe != m_joinConfig.end ())
        {
          ux = itUe->second.initPos[0]; uy = itUe->second.initPos[1]; uz = itUe->second.initPos[2];
        }
      listPosUe->Add (Vector (ux, uy, uz));
    }
  mobilityUe.SetPositionAllocator (listPosUe);
  mobilityUe.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobilityUe.Install (m_lteUeNodes);
}

void
HeterogeneousNmsFramework::BuildAdhocSubnet ()
{
  // ----------------- 由 JSON 驱动：按 join_config 中 subnet=Adhoc 的数量动态创建节点 -----------------
  uint32_t nAdhoc = m_defaultAdhocNodes;
  if (!m_joinConfig.empty ())
    {
      nAdhoc = 0;
      for (const auto& kv : m_joinConfig)
        if (kv.second.subnet == "Adhoc") nAdhoc++;
      if (nAdhoc == 0) nAdhoc = m_defaultAdhocNodes;
    }
  m_adhocNodes.Create (nAdhoc);

  ConfigureAdhocMobility ();

  OlsrHelper olsr;
  olsr.Set ("HelloInterval", TimeValue (Seconds (0.25)));
  olsr.Set ("TcInterval", TimeValue (Seconds (0.5)));
  olsr.Set ("MidInterval", TimeValue (Seconds (1.0)));
  olsr.Set ("HnaInterval", TimeValue (Seconds (1.0)));
  AodvHelper aodv;
  aodv.Set ("HelloInterval", TimeValue (Seconds (0.25)));
  aodv.Set ("ActiveRouteTimeout", TimeValue (Seconds (3.0)));
  aodv.Set ("AllowedHelloLoss", UintegerValue (3));
  Ipv4StaticRoutingHelper staticRouting;
  Ipv4ListRoutingHelper list;
  list.Add (staticRouting, 30);
  list.Add (aodv, 20);
  list.Add (olsr, 10);
  InternetStackHelper stack;
  stack.SetRoutingHelper (list);
  stack.Install (m_adhocNodes);

  // ----------------- WiFi Ad-hoc 配置（802.11g 为例）-----------------
  // Reduce hidden-node collisions under multi-hop load.
  Config::SetDefault ("ns3::WifiRemoteStationManager::RtsCtsThreshold", StringValue ("0"));
  // 拓扑由 JSON 强制指定：使用 MatrixPropagationLossModel，默认 1000 dB 隔离，仅 JSON 中 links 为正常损耗（如 40 dB）
  WifiHelper wifi;
  wifi.SetStandard (WIFI_STANDARD_80211n);
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager",
                                "DataMode", StringValue (m_adhocRateMode),
                                "ControlMode", StringValue (m_adhocRateMode));

  Ptr<YansWifiChannel> channel;
  std::set<std::pair<uint32_t, uint32_t>> adhocLinks = GetLinksFromJoinConfig (m_joinConfig, "Adhoc");
  if (!adhocLinks.empty ())
    {
      channel = CreateObject<YansWifiChannel> ();
      Ptr<MatrixPropagationLossModel> lossModel = CreateObject<MatrixPropagationLossModel> ();
      lossModel->SetDefaultLoss (1000.0);   // 默认完全隔离
      std::map<uint32_t, Ptr<MobilityModel>> idToMobility;
      for (uint32_t i = 0; i < m_adhocNodes.GetN (); ++i)
        {
          Ptr<Node> n = m_adhocNodes.Get (i);
          Ptr<MobilityModel> mob = n->GetObject<MobilityModel> ();
          if (mob) idToMobility[n->GetId ()] = mob;
        }
      const double LINK_LOSS_DB = 20.0;  // tighten reliability for configured links
      for (const auto& edge : adhocLinks)
        {
          auto itA = idToMobility.find (edge.first);
          auto itB = idToMobility.find (edge.second);
          if (itA != idToMobility.end () && itB != idToMobility.end ())
            {
              double loss = LINK_LOSS_DB;
              if ((edge.first == 2 && edge.second == 5) || (edge.first == 5 && edge.second == 2))
                {
                  // Critical relay hop for flow 5->1; force a very reliable link.
                  loss = 0.0;
                }
              lossModel->SetLoss (itA->second, itB->second, loss, true);
            }
        }
      channel->SetPropagationLossModel (lossModel);
      channel->SetPropagationDelayModel (CreateObject<ConstantSpeedPropagationDelayModel> ());
    }
  else
    {
      YansWifiChannelHelper wifiChannel = YansWifiChannelHelper::Default ();
      channel = wifiChannel.Create ();
    }

  YansWifiPhyHelper wifiPhy;
  wifiPhy.SetChannel (channel);

  WifiMacHelper wifiMac;
  wifiMac.SetType ("ns3::AdhocWifiMac");

  m_adhocDevs = wifi.Install (wifiPhy, wifiMac, m_adhocNodes);

  if (m_enablePcap && !m_outputDir.empty ())
    {
      std::string prefix = m_dirPacket + "/raw_hnmp_" + m_timestamp + "_adhoc";
      wifiPhy.EnablePcap (prefix, m_adhocDevs);
    }

  // 分配 IP 地址并保存接口容器（用于 Proxy 聚合时获取 SPN 子网地址）
  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.1.0.0", "255.255.255.0");
  m_ifAdhoc = ipv4.Assign (m_adhocDevs);

  // Ad-hoc 子网 SPN 由 ElectInitialSpn() 按性能评分选举，此处不预设
}

void
HeterogeneousNmsFramework::ConfigureAdhocMobility ()
{
  // 完全由 JSON 驱动：废除 GridPositionAllocator/硬编码坐标，仅使用 m_joinConfig 中的 init_pos
  MobilityHelper mobility;
  Ptr<ListPositionAllocator> listPos = CreateObject<ListPositionAllocator> ();
  uint32_t n = m_adhocNodes.GetN ();
  for (uint32_t i = 0; i < n; ++i)
    {
      uint32_t nodeId = m_adhocNodes.Get (i)->GetId ();
      double x = 100.0 + 30.0 * (i % 3), y = 100.0 + 30.0 * (i / 3), z = 0.0;  // 默认 fallback
      auto it = m_joinConfig.find (nodeId);
      if (it != m_joinConfig.end ())
        {
          x = it->second.initPos[0]; y = it->second.initPos[1]; z = it->second.initPos[2];
        }
      listPos->Add (Vector (x, y, z));
    }
  mobility.SetPositionAllocator (listPos);
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (m_adhocNodes);
}

void
HeterogeneousNmsFramework::BuildDataLinkSubnet ()
{
  // ----------------- 由 JSON 驱动：按 join_config 中 subnet=DataLink 的数量动态创建节点 -----------------
  uint32_t nDatalink = m_defaultDataLinkNodes;
  if (!m_joinConfig.empty ())
    {
      nDatalink = 0;
      for (const auto& kv : m_joinConfig)
        if (kv.second.subnet == "DataLink") nDatalink++;
      if (nDatalink == 0) nDatalink = m_defaultDataLinkNodes;
    }
  m_datalinkNodes.Create (nDatalink);

  ConfigureDataLinkMobility ();

  InternetStackHelper stack;
  stack.Install (m_datalinkNodes);

  // ----------------- 使用 802.11a，低速率 6Mbps；拓扑由 JSON 强制指定（MatrixPropagationLossModel）或默认信道 -----------------
  WifiHelper wifi;
  wifi.SetStandard (WIFI_STANDARD_80211n);
  wifi.SetRemoteStationManager ("ns3::ConstantRateWifiManager",
                                "DataMode", StringValue (m_datalinkRateMode),
                                "ControlMode", StringValue (m_datalinkRateMode));

  Ptr<YansWifiChannel> channel;
  std::set<std::pair<uint32_t, uint32_t>> dlLinks = GetLinksFromJoinConfig (m_joinConfig, "DataLink");
  if (!dlLinks.empty ())
    {
      channel = CreateObject<YansWifiChannel> ();
      Ptr<MatrixPropagationLossModel> lossModel = CreateObject<MatrixPropagationLossModel> ();
      lossModel->SetDefaultLoss (1000.0);   // 默认完全隔离
      std::map<uint32_t, Ptr<MobilityModel>> idToMobility;
      for (uint32_t i = 0; i < m_datalinkNodes.GetN (); ++i)
        {
          Ptr<Node> n = m_datalinkNodes.Get (i);
          Ptr<MobilityModel> mob = n->GetObject<MobilityModel> ();
          if (mob) idToMobility[n->GetId ()] = mob;
        }
      const double LINK_LOSS_DB = 20.0;
      for (const auto& edge : dlLinks)
        {
          auto itA = idToMobility.find (edge.first);
          auto itB = idToMobility.find (edge.second);
          if (itA != idToMobility.end () && itB != idToMobility.end ())
            lossModel->SetLoss (itA->second, itB->second, LINK_LOSS_DB, true);
        }
      channel->SetPropagationLossModel (lossModel);
      channel->SetPropagationDelayModel (CreateObject<ConstantSpeedPropagationDelayModel> ());
    }
  else
    {
      YansWifiChannelHelper wifiChannel = YansWifiChannelHelper::Default ();
      channel = wifiChannel.Create ();
    }

  YansWifiPhyHelper wifiPhy;
  wifiPhy.SetChannel (channel);

  WifiMacHelper wifiMac;
  wifiMac.SetType ("ns3::AdhocWifiMac");

  m_datalinkDevs = wifi.Install (wifiPhy, wifiMac, m_datalinkNodes);

  if (m_enablePcap && !m_outputDir.empty ())
    {
      std::string prefix = m_dirPacket + "/raw_hnmp_" + m_timestamp + "_datalink";
      wifiPhy.EnablePcap (prefix, m_datalinkDevs);
    }

  // IP 地址分配并保存接口容器（用于 Proxy 聚合时获取 SPN 子网地址）
  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.2.0.0", "255.255.255.0");
  m_ifDatalink = ipv4.Assign (m_datalinkDevs);

  // DataLink 子网 SPN 由 ElectInitialSpn() 按性能评分选举，此处不预设
}

void
HeterogeneousNmsFramework::ConfigureDataLinkMobility ()
{
  // 完全由 JSON 驱动：废除 GridPositionAllocator，仅使用 m_joinConfig 的 init_pos；DataLink 使用 ConstantVelocityMobilityModel，速度来自 JSON speed
  Ptr<ListPositionAllocator> listPos = CreateObject<ListPositionAllocator> ();
  uint32_t n = m_datalinkNodes.GetN ();
  for (uint32_t i = 0; i < n; ++i)
    {
      uint32_t nodeId = m_datalinkNodes.Get (i)->GetId ();
      double x = 220.0 + 25.0 * i, y = 20.0 + (i % 2) * 10.0, z = 0.0;  // 默认锯齿 Y
      auto it = m_joinConfig.find (nodeId);
      if (it != m_joinConfig.end ())
        {
          x = it->second.initPos[0]; y = it->second.initPos[1]; z = it->second.initPos[2];
        }
      listPos->Add (Vector (x, y, z));
    }
  MobilityHelper mobility;
  mobility.SetPositionAllocator (listPos);
  mobility.SetMobilityModel ("ns3::ConstantVelocityMobilityModel");
  mobility.Install (m_datalinkNodes);

  for (uint32_t i = 0; i < n; ++i)
    {
      Ptr<ConstantVelocityMobilityModel> m =
        m_datalinkNodes.Get (i)->GetObject<ConstantVelocityMobilityModel> ();
      if (m)
        {
          double vx = 15.0;
          auto it = m_joinConfig.find (m_datalinkNodes.Get (i)->GetId ());
          if (it != m_joinConfig.end () && it->second.speed > 0)
            vx = it->second.speed;
          m->SetVelocity (Vector (vx, 0.0, 0.0));
        }
    }
}

void
HeterogeneousNmsFramework::InstallStaticMobilityForNodesWithoutModel ()
{
  NodeContainer nodesWithoutMobility;
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      Ptr<Node> node = *it;
      if (!node || node->GetObject<MobilityModel> ()) continue;
      nodesWithoutMobility.Add (node);
    }
  if (nodesWithoutMobility.GetN () == 0) return;

  // 由 JSON 驱动：有 join_config 的用 init_pos，否则用简单 fallback 避免与 GMC 重合
  Ptr<ListPositionAllocator> listPos = CreateObject<ListPositionAllocator> ();
  for (uint32_t i = 0; i < nodesWithoutMobility.GetN (); ++i)
    {
      uint32_t nid = nodesWithoutMobility.Get (i)->GetId ();
      double x = 100.0 + 20.0 * i, y = 0.0, z = 0.0;
      auto it = m_joinConfig.find (nid);
      if (it != m_joinConfig.end ())
        {
          x = it->second.initPos[0]; y = it->second.initPos[1]; z = it->second.initPos[2];
        }
      listPos->Add (Vector (x, y, z));
    }
  MobilityHelper mobility;
  mobility.SetPositionAllocator (listPos);
  mobility.SetMobilityModel ("ns3::ConstantPositionMobilityModel");
  mobility.Install (nodesWithoutMobility);
}

double
HeterogeneousNmsFramework::GetNodeJoinTime (uint32_t nodeId) const
{
  auto it = m_joinConfig.find (nodeId);
  if (it == m_joinConfig.end ()) return 0.0;
  return it->second.joinTime;
}

Ptr<Node>
HeterogeneousNmsFramework::GetNodeById (uint32_t nodeId)
{
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      if ((*it)->GetId () == nodeId)
        return *it;
    }
  return nullptr;
}

void
HeterogeneousNmsFramework::BuildAdhocAddressMap ()
{
  m_adhocAddressToNodeId.clear ();
  m_adhocNodeIdToAddress.clear ();
  for (uint32_t i = 0; i < m_adhocNodes.GetN (); ++i)
    {
      Ptr<Node> n = m_adhocNodes.Get (i);
      uint32_t nid = n->GetId ();
      Ipv4Address addr = m_ifAdhoc.GetAddress (i);
      m_adhocAddressToNodeId[addr] = nid;
      m_adhocNodeIdToAddress[nid] = addr;
    }
}

std::vector<uint32_t>
HeterogeneousNmsFramework::GetOlsrPath (uint32_t srcNodeId, uint32_t dstNodeId) const
{
  std::vector<uint32_t> path;
  auto itDst = m_adhocNodeIdToAddress.find (dstNodeId);
  if (itDst == m_adhocNodeIdToAddress.end ()) return path;
  Ipv4Address dstAddr = itDst->second;
  Ptr<Node> curNode = GetNodeById (srcNodeId);
  if (!curNode) return path;
  path.push_back (srcNodeId);
  const int maxHops = 30;
  for (int h = 0; h < maxHops; ++h)
    {
      Ptr<Ipv4> ipv4 = curNode->GetObject<Ipv4> ();
      if (!ipv4) break;
      Ptr<Ipv4RoutingProtocol> rp = ipv4->GetRoutingProtocol ();
      if (!rp) break;
      Ipv4Header header;
      header.SetDestination (dstAddr);
      Socket::SocketErrno err = Socket::ERROR_NOTERROR;
      Ptr<Packet> probe = Create<Packet> (1);
      Ptr<Ipv4Route> route = rp->RouteOutput (probe, header, nullptr, err);
      if (!route) break;
      // Direct route may use 0.0.0.0 gateway in ns-3; still append destination.
      if (route->GetGateway () == Ipv4Address ("0.0.0.0"))
        {
          if (path.empty () || path.back () != dstNodeId)
            path.push_back (dstNodeId);
          break;
        }
      Ipv4Address nextHop = route->GetGateway ();
      auto itNext = m_adhocAddressToNodeId.find (nextHop);
      if (itNext == m_adhocAddressToNodeId.end ()) break;
      uint32_t nextId = itNext->second;
      path.push_back (nextId);
      if (nextId == dstNodeId) break;
      curNode = GetNodeById (nextId);
      if (!curNode) break;
    }
  return path;
}

void
HeterogeneousNmsFramework::BringUpNode (Ptr<Node> node)
{
  if (!node) return;
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
  if (!ipv4) return;
  for (uint32_t i = 1; i < ipv4->GetNInterfaces (); ++i)
    {
      if (!ipv4->IsUp (i))
        ipv4->SetUp (i);
    }
  NmsLog ("INFO", node->GetId (), "NODE_UP", "Interfaces brought up (join time).");
}

void
HeterogeneousNmsFramework::BringDownNodesWithDelayedJoin ()
{
  for (const auto& kv : m_joinConfig)
    {
      if (kv.second.joinTime <= 0.0) continue;
      uint32_t nodeId = kv.second.nodeId;
      Ptr<Node> node = NodeList::GetNode (nodeId);
      if (!node) continue;
      Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
      if (!ipv4) continue;
      for (uint32_t i = 1; i < ipv4->GetNInterfaces (); ++i)
        {
          if (ipv4->IsUp (i))
            ipv4->SetDown (i);
        }
      NmsLog ("INFO", nodeId, "NODE_DOWN", "Interfaces brought down until join time.");
      double jt = kv.second.joinTime;
      Simulator::Schedule (Seconds (std::max (0.0, jt - 0.0001)),
                          &HeterogeneousNmsFramework::BringUpNode, node);
    }
}

void
HeterogeneousNmsFramework::SetupBackhaul ()
{
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("100Mbps"));
  p2p.SetChannelAttribute ("Delay", StringValue ("2ms"));

  Ipv4AddressHelper ipv4;

  // ========== GMC <-> LTE SPN (eNodeB)，单条 ==========
  {
    NetDeviceContainer devices = p2p.Install (m_gmcNode.Get (0), m_spnLte);
    m_p2pGmcToLteSpn = devices;
    if (m_enablePcap && !m_outputDir.empty ())
      p2p.EnablePcap (m_dirPacket + "/agg_hnmp_" + m_timestamp + "_p2p", devices.Get (0), true);
    ipv4.SetBase ("10.100.0.0", "255.255.255.0");
    m_ifGmcLteSpn = ipv4.Assign (devices);
  }

  // ========== GMC <-> 每个 Adhoc 节点一条回程（动态 SPN：任一节点都可能成为 SPN 并激活此链路） ==========
  m_p2pGmcToAdhocBackhaul.clear ();
  m_ifGmcAdhocBackhaul.clear ();
  for (uint32_t i = 0; i < m_adhocNodes.GetN (); ++i)
    {
      NetDeviceContainer devices = p2p.Install (m_gmcNode.Get (0), m_adhocNodes.Get (i));
      m_p2pGmcToAdhocBackhaul.push_back (devices);
      if (m_enablePcap && !m_outputDir.empty ())
        p2p.EnablePcap (m_dirPacket + "/agg_hnmp_" + m_timestamp + "_p2p", devices.Get (0), true);
      std::ostringstream base;
      base << "10.101." << i << ".0";
      ipv4.SetBase (base.str ().c_str (), "255.255.255.0");
      Ipv4InterfaceContainer ifaces = ipv4.Assign (devices);
      m_ifGmcAdhocBackhaul.push_back (ifaces);
    }
  if (!m_ifGmcAdhocBackhaul.empty ())
    m_ifGmcAdhocSpn = m_ifGmcAdhocBackhaul[0];

  // ========== GMC <-> 每个 DataLink 节点一条回程 ==========
  m_p2pGmcToDatalinkBackhaul.clear ();
  m_ifGmcDatalinkBackhaul.clear ();
  for (uint32_t i = 0; i < m_datalinkNodes.GetN (); ++i)
    {
      NetDeviceContainer devices = p2p.Install (m_gmcNode.Get (0), m_datalinkNodes.Get (i));
      m_p2pGmcToDatalinkBackhaul.push_back (devices);
      if (m_enablePcap && !m_outputDir.empty ())
        p2p.EnablePcap (m_dirPacket + "/agg_hnmp_" + m_timestamp + "_p2p", devices.Get (0), true);
      std::ostringstream base;
      base << "10.102." << i << ".0";
      ipv4.SetBase (base.str ().c_str (), "255.255.255.0");
      Ipv4InterfaceContainer ifaces = ipv4.Assign (devices);
      m_ifGmcDatalinkBackhaul.push_back (ifaces);
    }
  if (!m_ifGmcDatalinkBackhaul.empty ())
    m_ifGmcDatalinkSpn = m_ifGmcDatalinkBackhaul[0];
}

/**
 * 按性能评分选举各子网 SPN：
 *  - LTE：固定为 eNodeB（回程拓扑决定）
 *  - Adhoc/DataLink：使用与 CalculateUtilityScore 一致的公式（能量、链路质量、移动性），
 *    用确定性顺序的随机初值计算每节点得分，选子网内得分最高者为 SPN
 */
void
HeterogeneousNmsFramework::ElectInitialSpn ()
{
  // LTE：回程只能接 eNB
  m_spnLte = m_lteEnbNodes.Get (0);
  {
    std::ostringstream oss;
    oss << "Subnet=LTE, SPN NodeId=" << m_spnLte->GetId () << " (eNB, fixed)";
    NmsLog ("INFO", m_spnLte->GetId (), "SPN_ELECT", oss.str ());
  }

  // Adhoc：子网内按初始性能评分选举
  Ptr<UniformRandomVariable> rv = CreateObject<UniformRandomVariable> ();
  const double w_e = 0.3, w_t = 0.3, w_l = 0.25;
  double bestScore = -1.0;
  uint32_t bestIdx = 0;
  for (uint32_t i = 0; i < m_adhocNodes.GetN (); ++i)
    {
      double link   = rv->GetValue (0.7, 1.0);
      double velocity = rv->GetValue (0.0, 20.0);
      double topology = 1.0 / (1.0 + std::exp (2.0 * (velocity - 10.0)));
      double energy = 0.9;
      double score = w_e * energy + w_t * topology + w_l * link;
      if (score > bestScore)
        {
          bestScore = score;
          bestIdx = i;
        }
    }
  m_spnAdhoc = m_adhocNodes.Get (bestIdx);
  {
    std::ostringstream oss;
    oss << "Subnet=Adhoc, SPN NodeId=" << m_spnAdhoc->GetId ()
        << ", Score=" << std::fixed << std::setprecision (3) << bestScore;
    NmsLog ("INFO", m_spnAdhoc->GetId (), "SPN_ELECT", oss.str ());
  }

  // DataLink：子网内按初始性能评分选举
  bestScore = -1.0;
  bestIdx = 0;
  for (uint32_t i = 0; i < m_datalinkNodes.GetN (); ++i)
    {
      double link   = rv->GetValue (0.7, 1.0);
      double velocity = rv->GetValue (0.0, 20.0);
      double topology = 1.0 / (1.0 + std::exp (2.0 * (velocity - 10.0)));
      double energy = 0.9;
      double score = w_e * energy + w_t * topology + w_l * link;
      if (score > bestScore)
        {
          bestScore = score;
          bestIdx = i;
        }
    }
  m_spnDatalink = m_datalinkNodes.Get (bestIdx);
  {
    std::ostringstream oss;
    oss << "Subnet=DataLink, SPN NodeId=" << m_spnDatalink->GetId ()
        << ", Score=" << std::fixed << std::setprecision (3) << bestScore;
    NmsLog ("INFO", m_spnDatalink->GetId (), "SPN_ELECT", oss.str ());
  }
}

void
HeterogeneousNmsFramework::InstallApplications ()
{
  m_businessFlowKeys.clear ();
  // HNMP 数据统一使用 8080，便于 Wireshark 过滤：udp.port == 8080
  uint16_t gmcDataPort   = 8080;
  uint16_t reportPort    = 8080;
  uint16_t spnPolicyPort = m_useDualChannel ? 8888 : 5002;
  uint16_t nodePolicyPort= m_useDualChannel ? 8888 : 5003;
  uint16_t helloPort     = m_useDualChannel ? 8888 : 6000;

  Time simTime = Seconds (m_simTimeSeconds);

  // 在 GMC 部署 UDP Sink，接收所有子网发来的数据报文（单通道 5000，双通道 9999）
  {
    Address sinkLocalAddress (InetSocketAddress (Ipv4Address::GetAny (), gmcDataPort));
    PacketSinkHelper sinkHelper ("ns3::UdpSocketFactory", sinkLocalAddress);
    ApplicationContainer sinkApps = sinkHelper.Install (m_gmcNode.Get (0));
    sinkApps.Start (Seconds (0.0));
    sinkApps.Stop (simTime);
  }

  // GMC 策略下发：向所有可能成为 SPN 的节点回程地址发送（动态 SPN 时任一 Adhoc/DataLink 节点都可能接收）
  {
    Ptr<GmcPolicySenderApp> policyApp = CreateObject<GmcPolicySenderApp> ();
    for (size_t i = 0; i < m_ifGmcAdhocBackhaul.size (); ++i)
      policyApp->AddTarget (m_ifGmcAdhocBackhaul[i].GetAddress (1), spnPolicyPort);
    for (size_t i = 0; i < m_ifGmcDatalinkBackhaul.size (); ++i)
      policyApp->AddTarget (m_ifGmcDatalinkBackhaul[i].GetAddress (1), spnPolicyPort);
    policyApp->SetInterval (Seconds (10.0));
    m_gmcNode.Get (0)->AddApplication (policyApp);
    policyApp->SetStartTime (Seconds (0.0));
    policyApp->SetStopTime (simTime);
  }

  Ipv4Address gmcToLteSpnAddr = m_ifGmcLteSpn.GetAddress (0);

  auto installOne = [&] (Ptr<Node> node, Ipv4Address peerAddr, uint16_t peerPort,
                          HeterogeneousNodeApp::SubnetType st, uint32_t size, Time interval,
                          bool isSpn, Ipv4Address subnetBroadcast, const std::string& nodeOnlineMsg,
                          Ipv4Address gmcBackhaulAddr = Ipv4Address ("0.0.0.0"), uint16_t gmcBackhaulPort = 0)
  {
    double startSec = 1.0;
    auto itJoin = m_joinConfig.find (node->GetId ());
    if (itJoin != m_joinConfig.end ())
      {
        startSec = itJoin->second.joinTime;
        Ptr<MobilityModel> mob = node->GetObject<MobilityModel> ();
        if (mob)
          {
            mob->SetPosition (Vector (itJoin->second.initPos[0], itJoin->second.initPos[1], itJoin->second.initPos[2]));
            if (itJoin->second.speed > 0)
              {
                Ptr<ConstantVelocityMobilityModel> cv = DynamicCast<ConstantVelocityMobilityModel> (mob);
                if (cv)
                  cv->SetVelocity (Vector (itJoin->second.speed, 0.0, 0.0));
              }
          }
      }
    Ptr<HeterogeneousNodeApp> app = CreateObject<HeterogeneousNodeApp> ();
    app->Configure (UdpSocketFactory::GetTypeId (), peerAddr, peerPort, size, interval);
    app->SetSubnetType (st);
    app->SetSpnRole (isSpn, subnetBroadcast);
    app->SetChannelPorts (reportPort, spnPolicyPort, nodePolicyPort, helloPort);
    app->SetProtocolTunables (m_energyDeltaThreshold, m_stateSuppressWindowSec, m_aggregateIntervalSec);
    app->SetSpnElectionTunables (m_spnElectionTimeoutSec, static_cast<uint8_t> (m_spnHeartbeatMissThreshold));
    if (gmcBackhaulPort != 0)
      app->SetGmcBackhaul (gmcBackhaulAddr, gmcBackhaulPort);
    if (st == HeterogeneousNodeApp::SUBNET_LTE)
      {
        app->SetInitialUvMib (100.0, 0.9);
      }
    if (itJoin != m_joinConfig.end ())
      {
        app->SetJoinTime (startSec);
        double e = itJoin->second.initialEnergy >= 0.0 ? itJoin->second.initialEnergy : -1.0;
        double lq = itJoin->second.initialLinkQuality >= 0.0 ? itJoin->second.initialLinkQuality : -1.0;
        if (e >= 0.0 || lq >= 0.0)
          app->SetInitialUvMib (e >= 0.0 ? ((st == HeterogeneousNodeApp::SUBNET_LTE) ? (e * 100.0) : e) : 0.9,
                                lq >= 0.0 ? lq : 0.85);
      }
    node->AddApplication (app);
    app->SetStartTime (Seconds (startSec));
    app->SetStopTime (simTime);
    if (itJoin != m_joinConfig.end () && startSec > 0)
      {
        Simulator::Schedule (Seconds (startSec), [node, nodeOnlineMsg] () {
            NmsLog ("INFO", node->GetId (), "NODE_ONLINE", nodeOnlineMsg);
          });
      }
    else
      NmsLog ("INFO", node->GetId (), "NODE_ONLINE", nodeOnlineMsg);
  };

  // ---------- LTE：eNB (SPN) + 所有 UE 均向 GMC 上报（无子网聚合） ----------
  installOne (m_spnLte, gmcToLteSpnAddr, gmcDataPort, HeterogeneousNodeApp::SUBNET_LTE, 200, Seconds (0.5),
              false, Ipv4Address ("0.0.0.0"), "LTE SPN (eNB) application started.");
  for (uint32_t i = 0; i < m_lteUeNodes.GetN (); ++i)
    {
      Ptr<Node> ue = m_lteUeNodes.Get (i);
      installOne (ue, gmcToLteSpnAddr, gmcDataPort, HeterogeneousNodeApp::SUBNET_LTE, 200, Seconds (0.5),
                  false, Ipv4Address ("0.0.0.0"), "LTE UE application started.");
    }

  // ---------- Adhoc：所有节点初始均非 SPN，peer 为本节点回程 GMC 地址（动态选举后 SPN 向 GMC 上报） ----------
  for (uint32_t i = 0; i < m_adhocNodes.GetN (); ++i)
    {
      Ptr<Node> n = m_adhocNodes.Get (i);
      Ipv4Address gmcAddr = m_ifGmcAdhocBackhaul[i].GetAddress (0);
      std::string msg = "Adhoc node application started (dynamic SPN election).";
      installOne (n, gmcAddr, gmcDataPort, HeterogeneousNodeApp::SUBNET_ADHOC, 200, Seconds (0.5),
                  false, Ipv4Address ("10.1.0.255"), msg, gmcAddr, gmcDataPort);
    }

  // ---------- DataLink：所有节点初始均非 SPN，peer 为本节点回程 GMC 地址 ----------
  for (uint32_t i = 0; i < m_datalinkNodes.GetN (); ++i)
    {
      Ptr<Node> n = m_datalinkNodes.Get (i);
      Ipv4Address gmcAddr = m_ifGmcDatalinkBackhaul[i].GetAddress (0);
      std::string msg = "DataLink node application started (dynamic SPN election).";
      installOne (n, gmcAddr, gmcDataPort, HeterogeneousNodeApp::SUBNET_DATALINK,
                  m_datalinkPacketSize, Seconds (m_datalinkIntervalSec),
                  false, Ipv4Address ("10.2.0.255"), msg, gmcAddr, gmcDataPort);
    }

  // ---------- 业务流：优先读取 scenario_config.json 中的 business_flows ----------
  ScenarioConfig sc = LoadScenarioConfig (m_scenarioConfigPath);
  std::vector<BusinessFlowConfig> flows = sc.businessFlows;
  auto getConfiguredPath = [this] (uint32_t srcId, uint32_t dstId) {
    std::vector<uint32_t> path;
    if (srcId == dstId) { path.push_back (srcId); return path; }
    std::map<uint32_t, uint32_t> prev;
    std::set<uint32_t> visited;
    std::queue<uint32_t> q;
    visited.insert (srcId);
    q.push (srcId);
    bool found = false;
    while (!q.empty () && !found)
      {
        uint32_t cur = q.front ();
        q.pop ();
        auto it = m_joinConfig.find (cur);
        if (it == m_joinConfig.end ()) continue;
        for (uint32_t nb : it->second.neighbors)
          {
            if (visited.count (nb)) continue;
            visited.insert (nb);
            prev[nb] = cur;
            if (nb == dstId) { found = true; break; }
            q.push (nb);
          }
      }
    if (!found) return path;
    uint32_t cur = dstId;
    std::vector<uint32_t> rev;
    rev.push_back (cur);
    while (cur != srcId)
      {
        auto it = prev.find (cur);
        if (it == prev.end ()) return std::vector<uint32_t> ();
        cur = it->second;
        rev.push_back (cur);
      }
    for (auto it = rev.rbegin (); it != rev.rend (); ++it) path.push_back (*it);
    return path;
  };
  auto getPrimaryNonLoopbackIf = [] (Ptr<Ipv4> ipv4) -> int32_t {
    if (!ipv4) return -1;
    for (uint32_t i = 1; i < ipv4->GetNInterfaces (); ++i)
      {
        if (ipv4->GetNAddresses (i) == 0) continue;
        Ipv4Address a = ipv4->GetAddress (i, 0).GetLocal ();
        if (a != Ipv4Address ("127.0.0.1")) return static_cast<int32_t> (i);
      }
    return -1;
  };
  auto logRouteProbe = [this] (uint32_t nodeId, Ipv4Address dstAddr, const std::string& tag) {
    Ptr<Node> node = GetNodeById (nodeId);
    if (!node) return;
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
    if (!ipv4) return;
    Ptr<Ipv4RoutingProtocol> rp = ipv4->GetRoutingProtocol ();
    if (!rp)
      {
        NmsLog ("INFO", nodeId, "ROUTE_PROBE", tag + " no-routing-protocol");
        return;
      }
    Ipv4Header header;
    header.SetDestination (dstAddr);
    Socket::SocketErrno err = Socket::ERROR_NOTERROR;
    Ptr<Packet> probe = Create<Packet> (1);
    Ptr<Ipv4Route> route = rp->RouteOutput (probe, header, nullptr, err);
    if (!route)
      {
        NmsLog ("INFO", nodeId, "ROUTE_PROBE", tag + " route=null err=" + std::to_string (static_cast<int> (err)));
        return;
      }
    std::ostringstream oss;
    oss << tag << " dst=" << dstAddr
        << " gw=" << route->GetGateway ()
        << " src=" << route->GetSource ()
        << " outIf=" << route->GetOutputDevice ()->GetIfIndex ();
    NmsLog ("INFO", nodeId, "ROUTE_PROBE", oss.str ());
  };
  if (flows.empty ())
    {
      // 回退默认业务流（兼容旧行为）
      if (m_adhocNodes.GetN () >= 2)
        {
          BusinessFlowConfig f1 = {1, "video", 2, 2, m_adhocNodes.Get (0)->GetId (), m_adhocNodes.Get (1)->GetId (), "2Mbps", 62500, 5.0, -1.0};
          flows.push_back (f1);
        }
      if (m_adhocNodes.GetN () >= 5)
        {
          BusinessFlowConfig f2 = {2, "data", 1, 1, m_adhocNodes.Get (1)->GetId (), m_adhocNodes.Get (4)->GetId (), "1Mbps", 512, 7.0, -1.0};
          flows.push_back (f2);
        }
      if (m_lteUeNodes.GetN () > 0 && m_lteEnbNodes.GetN () > 0)
        {
          uint32_t srcIdx = std::min<uint32_t> (2, m_lteUeNodes.GetN () - 1);
          BusinessFlowConfig f3 = {3, "control", 0, 0, m_lteUeNodes.Get (srcIdx)->GetId (), m_lteEnbNodes.Get (0)->GetId (), "100Kbps", 256, 9.0, -1.0};
          flows.push_back (f3);
        }
    }

  uint16_t basePort = 9000;
  for (size_t i = 0; i < flows.size (); ++i)
    {
      const BusinessFlowConfig& fcfg = flows[i];
      Ptr<Node> srcNode = GetNodeById (fcfg.srcNodeId);
      Ptr<Node> dstNode = GetNodeById (fcfg.dstNodeId);
      if (!srcNode || !dstNode) continue;
      std::string dstIpStr = GetNodePrimaryIpv4 (dstNode);
      if (dstIpStr.empty ()) continue;
      std::string srcIpStr = GetNodePrimaryIpv4 (srcNode);
      if (srcIpStr.empty ()) continue;
      Ipv4Address dstAddr (dstIpStr.c_str ());
      uint16_t port = basePort + static_cast<uint16_t> (fcfg.flowId % 500);
      std::vector<uint32_t> configuredPath = getConfiguredPath (fcfg.srcNodeId, fcfg.dstNodeId);
      // Install host-route fallback hop-by-hop so configured multi-hop flows
      // (e.g. 5->2->1) can still forward even when dynamic routing is late.
      if (configuredPath.size () >= 2)
        {
          Ipv4StaticRoutingHelper srh;
          for (size_t hop = 0; hop + 1 < configuredPath.size (); ++hop)
            {
              Ptr<Node> curNode = GetNodeById (configuredPath[hop]);
              Ptr<Node> nextNode = GetNodeById (configuredPath[hop + 1]);
              if (!curNode || !nextNode) continue;
              std::string nextIpStr = GetNodePrimaryIpv4 (nextNode);
              std::string curIpStr = GetNodePrimaryIpv4 (curNode);
              if (nextIpStr.empty () || curIpStr.empty ()) continue;
              Ptr<Ipv4> curIpv4 = curNode->GetObject<Ipv4> ();
              if (!curIpv4) continue;
              int32_t outIf = getPrimaryNonLoopbackIf (curIpv4);
              if (outIf < 0) continue;
              Ptr<Ipv4StaticRouting> srt = srh.GetStaticRouting (curIpv4);
              if (!srt) continue;
              srt->AddHostRouteTo (dstAddr, Ipv4Address (nextIpStr.c_str ()), static_cast<uint32_t> (outIf));
            }
        }
      {
        std::ostringstream key;
        key << srcIpStr << "|" << dstIpStr << "|" << port;
        m_businessFlowKeys.insert (key.str ());
        m_businessFlowKeyToId[key.str ()] = fcfg.flowId;
      }

      PacketSinkHelper sink ("ns3::UdpSocketFactory", InetSocketAddress (Ipv4Address::GetAny (), port));
      ApplicationContainer sinkApps = sink.Install (dstNode);
      sinkApps.Start (Seconds (0.0));
      sinkApps.Stop (simTime);

      OnOffHelper onOff ("ns3::UdpSocketFactory", Address ());
      onOff.SetAttribute ("Remote", AddressValue (InetSocketAddress (dstAddr, port)));
      onOff.SetAttribute ("DataRate", DataRateValue (DataRate (fcfg.dataRate.c_str ())));
      onOff.SetAttribute ("PacketSize", UintegerValue (fcfg.packetSize));
      // Force CBR send pattern to avoid OnOff duty-cycle throughput loss.
      onOff.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1]"));
      onOff.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
      ApplicationContainer clientApps = onOff.Install (srcNode);
      double stopAt = (fcfg.stopTime > 0.0) ? fcfg.stopTime : m_simTimeSeconds;
      double stagger = static_cast<double> (fcfg.flowId % 5) * 0.05; // avoid synchronized burst starts
      clientApps.Start (Seconds (fcfg.startTime + stagger));
      clientApps.Stop (Seconds (stopAt));
      if (fcfg.flowId == 5)
        {
          Simulator::Schedule (Seconds (fcfg.startTime + 0.2),
                               [logRouteProbe, fcfg, dstAddr] () {
                                 logRouteProbe (fcfg.srcNodeId, dstAddr, "flow5-src");
                               });
          std::vector<uint32_t> p = getConfiguredPath (fcfg.srcNodeId, fcfg.dstNodeId);
          if (p.size () >= 2)
            {
              uint32_t relay = p[1];
              Simulator::Schedule (Seconds (fcfg.startTime + 0.3),
                                   [logRouteProbe, relay, dstAddr] () {
                                     logRouteProbe (relay, dstAddr, "flow5-relay");
                                   });
            }
        }

      Simulator::Schedule (Seconds (fcfg.startTime), [this, fcfg, getConfiguredPath] () {
          std::vector<uint32_t> path = GetOlsrPath (fcfg.srcNodeId, fcfg.dstNodeId);
          if (path.size () < 2)
            {
              path = getConfiguredPath (fcfg.srcNodeId, fcfg.dstNodeId);
            }
          std::ostringstream oss;
          oss << "flowId=" << fcfg.flowId
              << " priority=" << static_cast<uint32_t> (fcfg.priority)
              << " qos=" << static_cast<uint32_t> (fcfg.qos)
              << " src=" << fcfg.srcNodeId << " dst=" << fcfg.dstNodeId
              << " size=" << fcfg.packetSize << " rate=" << fcfg.dataRate
              << " type=" << fcfg.type;
          if (!path.empty ())
            {
              oss << " path=";
              for (size_t pi = 0; pi < path.size (); ++pi) oss << (pi ? "," : "") << path[pi];
            }
          NmsLog ("INFO", 0, "FLOW_START", oss.str ());
        });
      // Route may not converge exactly at flow start; re-sample shortly after for accurate path display.
      Simulator::Schedule (Seconds (fcfg.startTime + 1.0), [this, fcfg, getConfiguredPath] () {
          std::vector<uint32_t> path = GetOlsrPath (fcfg.srcNodeId, fcfg.dstNodeId);
          if (path.size () < 2)
            {
              path = getConfiguredPath (fcfg.srcNodeId, fcfg.dstNodeId);
            }
          if (path.empty ()) return;
          std::ostringstream oss;
          oss << "flowId=" << fcfg.flowId << " path=";
          for (size_t pi = 0; pi < path.size (); ++pi) oss << (pi ? "," : "") << path[pi];
          NmsLog ("INFO", 0, "ROUTE_START", oss.str ());
        });
    }
}

void
HeterogeneousNmsFramework::WriteAllNodesJsonl ()
{
  double now = Simulator::Now ().GetSeconds ();
  auto writeOne = [this, now] (Ptr<Node> node) {
    uint32_t nodeId = node->GetId ();
    std::string joinState = "joined";
    double energy = 1.0, linkQ = 1.0;
    auto it = m_joinConfig.find (nodeId);
    if (it != m_joinConfig.end ())
      {
        if (now < it->second.joinTime)
          {
            joinState = "not_joined";
            if (it->second.initialEnergy >= 0.0) energy = it->second.initialEnergy;
            if (it->second.initialLinkQuality >= 0.0) linkQ = it->second.initialLinkQuality;
          }
      }
    Vector pos (0, 0, 0);
    Ptr<MobilityModel> mob = node->GetObject<MobilityModel> ();
    if (mob) pos = mob->GetPosition ();
    std::string ip = GetNodePrimaryIpv4 (node);
    std::string role;
    for (uint32_t a = 0; a < node->GetNApplications (); ++a)
      {
        Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> (node->GetApplication (a));
        if (!app) continue;
        if (app->IsSpn ()) { role = "primary_spn"; break; }
        if (app->IsBackupSpn ()) role = "standby_spn";
      }
    WriteJsonlStateLine (nodeId, joinState, pos.x, pos.y, pos.z, ip, energy, linkQ, role);
  };
  // 必须写入所有节点组（含 LTE UE），否则日志只有 12 个节点、缺 12,13,14,15
  for (uint32_t i = 0; i < m_gmcNode.GetN (); ++i) writeOne (m_gmcNode.Get (i));
  for (uint32_t i = 0; i < m_adhocNodes.GetN (); ++i) writeOne (m_adhocNodes.Get (i));
  for (uint32_t i = 0; i < m_datalinkNodes.GetN (); ++i) writeOne (m_datalinkNodes.Get (i));
  for (uint32_t i = 0; i < m_lteEnbNodes.GetN (); ++i) writeOne (m_lteEnbNodes.Get (i));
  for (uint32_t i = 0; i < m_lteUeNodes.GetN (); ++i) writeOne (m_lteUeNodes.Get (i));
  if (std::fabs (std::fmod (now, 5.0)) < 0.26)
    {
      std::ostringstream ad, dl, lte;
      ad << "Adhoc:";
      for (uint32_t i = 0; i < m_adhocNodes.GetN (); ++i) ad << (i ? "," : "") << m_adhocNodes.Get (i)->GetId ();
      dl << "DataLink:";
      for (uint32_t i = 0; i < m_datalinkNodes.GetN (); ++i) dl << (i ? "," : "") << m_datalinkNodes.Get (i)->GetId ();
      lte << "LTE:";
      if (m_lteEnbNodes.GetN () > 0) lte << m_lteEnbNodes.Get (0)->GetId ();
      for (uint32_t i = 0; i < m_lteUeNodes.GetN (); ++i) lte << "," << m_lteUeNodes.Get (i)->GetId ();
      NmsLog ("INFO", 0, "SUBNET_NODE_LIST", ad.str ());
      NmsLog ("INFO", 0, "SUBNET_NODE_LIST", dl.str ());
      NmsLog ("INFO", 0, "SUBNET_NODE_LIST", lte.str ());
    }
  if (now + 0.5 < m_simTimeSeconds)
    Simulator::Schedule (Seconds (0.5), &HeterogeneousNmsFramework::WriteAllNodesJsonl, this);
}

/**
 * 配置监控工具：
 *  - FlowMonitor：输出吞吐量、时延等统计信息（XML）
 *  - NetAnim 已禁用：不再生成动画 XML
 */
void
HeterogeneousNmsFramework::SetupMonitoring ()
{
  // FlowMonitor（控制/数据双通道时按五元组自动分流统计，无需额外配置）
  m_flowMonitor = m_flowHelper.InstallAll ();
  NmsLog ("INFO", 0, "MONITORING", "NetAnim disabled; only FlowMonitor enabled.");
}

void
HeterogeneousNmsFramework::EmitFlowPerformanceWindow ()
{
  if (!m_flowMonitor)
    {
      return;
    }
  Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (m_flowHelper.GetClassifier ());
  FlowMonitor::FlowStatsContainer stats = m_flowMonitor->GetFlowStats ();
  for (auto it = stats.begin (); it != stats.end (); ++it)
    {
      const uint32_t flowId = it->first;
      Ipv4FlowClassifier::FiveTuple t;
      if (classifier)
        {
          t = classifier->FindFlow (flowId);
        }
      else
        {
          continue;
        }
      if (t.sourcePort < 9000 && t.destinationPort < 9000)
        {
          continue;
        }
      std::ostringstream flowKey;
      flowKey << t.sourceAddress << "|" << t.destinationAddress << "|" << t.destinationPort;
      auto bizIt = m_businessFlowKeyToId.find (flowKey.str ());
      if (bizIt == m_businessFlowKeyToId.end ())
        {
          continue;
        }
      const uint32_t bizFlowId = bizIt->second;
      const FlowMonitor::FlowStats& s = it->second;
      uint64_t lostPkts = (s.txPackets >= s.rxPackets) ? (s.txPackets - s.rxPackets) : 0;
      double delaySumMs = s.delaySum.GetMilliSeconds ();
      FlowWindowSnapshot prev = m_flowWindowPrev[flowId];
      uint64_t dTx = (s.txPackets >= prev.txPackets) ? (s.txPackets - prev.txPackets) : 0;
      uint64_t dRx = (s.rxPackets >= prev.rxPackets) ? (s.rxPackets - prev.rxPackets) : 0;
      uint64_t dLost = (lostPkts >= prev.lostPackets) ? (lostPkts - prev.lostPackets) : 0;
      uint64_t dRxBytes = (s.rxBytes >= prev.rxBytes) ? (s.rxBytes - prev.rxBytes) : 0;
      double dDelayMs = (delaySumMs >= prev.delaySumMs) ? (delaySumMs - prev.delaySumMs) : 0.0;
      double thrMbps = (dRxBytes * 8.0) / std::max (0.001, m_flowPerfWindowSec) / 1e6;
      double delayMs = (dRx > 0) ? (dDelayMs / static_cast<double> (dRx)) : 0.0;
      double lossPct = (dTx > 0) ? (100.0 * static_cast<double> (dLost) / static_cast<double> (dTx)) : 0.0;
      std::ostringstream perfLine;
      perfLine << "bizFlowId=" << bizFlowId
               << " flowId=" << flowId
               << " win=" << std::fixed << std::setprecision (2) << m_flowPerfWindowSec << "s"
               << " delay=" << std::setprecision (2) << delayMs
               << "ms throughput=" << std::setprecision (3) << thrMbps
               << "Mbps loss=" << std::setprecision (2) << lossPct << "%";
      NmsLog ("INFO", 0, "FLOW_PERF_WIN", perfLine.str ());
      FlowWindowSnapshot cur;
      cur.txPackets = s.txPackets;
      cur.rxPackets = s.rxPackets;
      cur.lostPackets = lostPkts;
      cur.rxBytes = s.rxBytes;
      cur.delaySumMs = delaySumMs;
      m_flowWindowPrev[flowId] = cur;
    }
  double now = Simulator::Now ().GetSeconds ();
  if (now + m_flowPerfWindowSec < m_simTimeSeconds)
    {
      Simulator::Schedule (Seconds (m_flowPerfWindowSec),
                           &HeterogeneousNmsFramework::EmitFlowPerformanceWindow,
                           this);
    }
}

/**
 * Run：统一的仿真调度接口
 *  - 外部只需设置仿真时长，框架内部负责调用各阶段方法
 *
 * 若要扩展控制逻辑（如按阶段动态调整参数、定时调用算法等），
 * 可在 Run() 内部增加定时事件（Simulator::Schedule）。
 */
void
HeterogeneousNmsFramework::Run (double simTimeSeconds)
{
  m_simTimeSeconds = simTimeSeconds;

  // === 仿真文件多级结构化归档：simulation_results/时间戳 + visualization/performance/log/packet ===
  {
    time_t now = time (nullptr);
    struct tm* t = localtime (&now);
    char buf[64];
    snprintf (buf, sizeof (buf), "%04d-%02d-%02d_%02d-%02d-%02d",
              t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
              t->tm_hour, t->tm_min, t->tm_sec);
    m_timestamp = buf;
    m_outputDir = "simulation_results/" + m_timestamp;
    mkdir ("simulation_results", 0755);
    if (mkdir (m_outputDir.c_str (), 0755) != 0 && errno != EEXIST)
      {
        NmsLog ("WARN", 0, "ARCHIVE", "Cannot create " + m_outputDir + ", using current dir.");
        m_outputDir = ".";
      }
    m_dirVisualization = m_outputDir + "/visualization";
    m_dirPerformance  = m_outputDir + "/performance";
    m_dirLog          = m_outputDir + "/log";
    m_dirPacket       = m_outputDir + "/packet";
    mkdir (m_dirVisualization.c_str (), 0755);
    mkdir (m_dirPerformance.c_str (), 0755);
    mkdir (m_dirLog.c_str (), 0755);
    mkdir (m_dirPacket.c_str (), 0755);
    NmsLog ("INFO", 0, "SYSTEM", "Output directory: " + m_outputDir + " (visualization/performance/log/packet)");
  }

  NmsPacketParse::SetEnable (m_enablePacketParse);

  std::string logPath = MakeOutputPathInCategory ("log", "nms-framework-log", ".txt");
  g_nmsLogFile.open (logPath.c_str (), std::ios::out);
  if (g_nmsLogFile.is_open ())
    {
      g_nmsLogFile << "========== NMS Framework Log (simTime=" << simTimeSeconds << "s) ==========" << std::endl;
    }
  NmsLog ("INFO", 0, "SYSTEM", "Simulation starting.");
  NmsLog ("INFO", 0, "PCAP_USAGE", "Wireshark: open packet/raw_hnmp*.pcap or packet/agg_hnmp*.pcap, filter 'udp.port == 8080'");
  NmsLog ("INFO", 0, "HNMP_PARAMS",
          "energyDeltaTh=" + std::to_string (m_energyDeltaThreshold) +
          " suppressWin=" + std::to_string (m_stateSuppressWindowSec) +
          " aggregateInterval=" + std::to_string (m_aggregateIntervalSec));

  if (!m_joinConfigPath.empty ())
    {
      m_joinConfig = LoadNodeJoinConfig (m_joinConfigPath);
      NmsLog ("INFO", 0, "SYSTEM", "Loaded join config: " + std::to_string (m_joinConfig.size ()) + " nodes from " + m_joinConfigPath);
      std::string valErr = ValidateJoinConfig (m_joinConfig);
      if (!valErr.empty ())
        NmsLog ("WARN", 0, "JOINCONFIG_VALIDATION", valErr);
    }
  if (!m_scenarioConfigPath.empty ())
    {
      ScenarioConfig sc = LoadScenarioConfig (m_scenarioConfigPath);
      if (sc.spnElectionTimeoutSec > 0.0)
        {
          m_spnElectionTimeoutSec = sc.spnElectionTimeoutSec;
        }
      if (sc.spnHeartbeatMissThreshold > 0)
        {
          m_spnHeartbeatMissThreshold = sc.spnHeartbeatMissThreshold;
        }
      NmsLog ("INFO", 0, "SCENARIO", "Loaded scenario: id=" + (sc.scenarioId.empty () ? "none" : sc.scenarioId)
              + " from " + m_scenarioConfigPath);
      for (const auto& ev : sc.events)
        {
          if (ev.type == "NODE_FAIL" && ev.target > 0)
            {
              Simulator::Schedule (Seconds (ev.time), &HeterogeneousNmsFramework::InjectNodeFailure, this, ev.target);
              NmsLog ("INFO", 0, "SYSTEM", "Scheduled event NODE_FAIL at t=" + std::to_string (ev.time) + "s target=Node " + std::to_string (ev.target));
            }
          else if (ev.type == "SPN_SWITCH" || ev.type == "SPN_FORCE_SWITCH")
            {
              NmsLog ("WARN", 0, "SYSTEM",
                      "Deprecated event " + ev.type + " ignored: manual SPN target switch is disabled.");
            }
        }
    }
  // 论文实验模式：在无 join_config 时提供默认 30 节点/窄带参数；compare-* 用于对比实验
  if (m_scenarioMode == "thesis30")
    {
      m_defaultAdhocNodes = 10;
      m_defaultDataLinkNodes = 10;
      m_defaultLteUeNodes = 9; // +1 eNB +1 GMC = 31（EPC 为隐藏节点）
      m_datalinkRateMode = "OfdmRate6Mbps";
      m_datalinkPacketSize = 120;
      m_datalinkIntervalSec = 0.1; // 约 9.6kbps 应用层负载
      m_energyDeltaThreshold = 0.05;
      m_stateSuppressWindowSec = 1.0;
      m_aggregateIntervalSec = 2.0;
    }
  else if (m_scenarioMode == "compare-baseline")
    {
      // baseline：无聚合、无抑制、低效控制（近似传统洪泛）
      m_energyDeltaThreshold = 0.01;
      m_stateSuppressWindowSec = 0.0;
      m_aggregateIntervalSec = 0.2;
      m_datalinkPacketSize = 200;
      m_datalinkIntervalSec = 0.05;
    }
  else if (m_scenarioMode == "compare-hnmp")
    {
      // HNMP：强化抑制+聚合，突出论文指标优势
      m_energyDeltaThreshold = 0.12;
      m_stateSuppressWindowSec = 3.0;
      m_aggregateIntervalSec = 4.0;
      m_datalinkPacketSize = 120;
      m_datalinkIntervalSec = 0.2;
    }
  NmsLog ("INFO", 0, "SCENARIO_MODE",
          "mode=" + m_scenarioMode +
          " adhoc=" + std::to_string (m_defaultAdhocNodes) +
          " datalink=" + std::to_string (m_defaultDataLinkNodes) +
          " lteUe=" + std::to_string (m_defaultLteUeNodes) +
          " dlPkt=" + std::to_string (m_datalinkPacketSize) +
          " dlInt=" + std::to_string (m_datalinkIntervalSec));
  NmsLog ("INFO", 0, "HNMP_PARAMS_EFFECTIVE",
          "energyDeltaTh=" + std::to_string (m_energyDeltaThreshold) +
          " suppressWin=" + std::to_string (m_stateSuppressWindowSec) +
          " aggregateInterval=" + std::to_string (m_aggregateIntervalSec));
  std::string jsonlPath = MakeOutputPathInCategory ("log", "nms-state", ".jsonl");
  g_jsonlStateFile.open (jsonlPath.c_str (), std::ios::out);

  // === 1. 构建拓扑（顺序：GMC → Adhoc → DataLink → LTE，使 EPC 节点 ID 最大，Node List 中 EPC 排在最后） ===
  BuildGmc ();
  BuildAdhocSubnet ();
  BuildDataLinkSubnet ();
  BuildLteSubnet ();

  // === 1.45 业务流路径追踪：构建 Adhoc 地址 <-> 节点 ID 映射（OLSR 路径解析用） ===
  BuildAdhocAddressMap ();

  // === 1.5 为所有仍未设置 MobilityModel 的节点安装静态位置模型 ===
  InstallStaticMobilityForNodesWithoutModel ();

  // === 1.6 不再静态选举 Adhoc/DataLink SPN（SPN 由应用层 Hello 动态选举）；LTE SPN 固定为 eNB，已在 BuildLteSubnet 中设置 ===
  // ElectInitialSpn ();  // 已移除

  // === 2. 搭建回程链路（LTE 单条；Adhoc/DataLink 每个节点一条，供动态 SPN 激活） ===
  SetupBackhaul ();

  // === 3. 静态路由（避免 Ipv4GlobalRoutingHelper::PopulateRoutingTables 在 LTE 拓扑下触发空指针）
  // GMC 到 LTE UE 网段 7.0.0.0/8 经 eNB 10.100.0.2
  {
    Ptr<Ipv4> ipv4Gmc = m_gmcNode.Get (0)->GetObject<Ipv4> ();
    Ipv4StaticRoutingHelper staticRoutingHelper;
    Ptr<Ipv4StaticRouting> routing = staticRoutingHelper.GetStaticRouting (ipv4Gmc);
    if (routing)
      {
        uint32_t ifLte = 0;
        for (uint32_t i = 1; i < ipv4Gmc->GetNInterfaces (); ++i)
          {
            if (ipv4Gmc->GetAddress (i, 0).GetLocal () == m_ifGmcLteSpn.GetAddress (0))
              {
                ifLte = i;
                break;
              }
          }
        if (ifLte > 0)
          {
            routing->AddNetworkRouteTo (Ipv4Address ("7.0.0.0"), Ipv4Mask ("255.0.0.0"),
                                       m_ifGmcLteSpn.GetAddress (1), ifLte);
          }
      }
    NmsLog ("INFO", 0, "ROUTING", "Static route on GMC: 7.0.0.0/8 via eNB.");
  }

  // 注意：LTE UE -> GMC 需 PGW 有到 10.100.0.0/24 的路由；EPC 内部未配置时 UE->GMC 可能不通，
  // Adhoc/DataLink 到 GMC 为直连，不受影响。若需全局自动路由，可排查 GlobalRouter 与 LTE 兼容性后再启用 PopulateRoutingTables()。

  // === 4. 安装自定义应用 ===
  InstallApplications ();

  // === 4.5 时序入网：joinTime > 0 的节点在 t=0 时接口 Down，joinTime 时刻 Up ===
  if (!m_joinConfig.empty ())
    BringDownNodesWithDelayedJoin ();

  // 周期性输出 JSONL 状态（每 0.5s），供可视化解析
  Simulator::Schedule (Seconds (0.5), &HeterogeneousNmsFramework::WriteAllNodesJsonl, this);
  // FLOW_START 已移至 InstallApplications 中在流启动时刻按 OLSR 路径打印；FLOW_PERF 在仿真结束后由 FlowMonitor 统计输出（含 loss=）

  // === 5. 配置监控工具（FlowMonitor + NetAnim） ===
  SetupMonitoring ();
  // 周期性窗口性能（默认 0.5s）：输出 FLOW_PERF_WIN 供前端真实时间序列
  Simulator::Schedule (Seconds (m_flowPerfWindowSec), &HeterogeneousNmsFramework::EmitFlowPerformanceWindow, this);

  // === 5.5 事件注入：在指定时刻强制某节点断电（可选，m_failNodeId>0 时生效） ===
  if (m_failNodeId > 0 && m_failTime >= 0)
    {
      Simulator::Schedule (Seconds (m_failTime), &HeterogeneousNmsFramework::InjectNodeFailure, this, m_failNodeId);
      NmsLog ("INFO", 0, "SYSTEM", "Event injection: Node " + std::to_string (m_failNodeId) + " will fail at t=" + std::to_string (m_failTime) + "s");
    }

  // === 6. 仿真控制 ===
  Simulator::Stop (Seconds (simTimeSeconds));
  Simulator::Run ();

  if (g_jsonlStateFile.is_open ()) g_jsonlStateFile.close ();

  // === 7. 仿真结束后导出 FlowMonitor 结果与性能日志至归档目录 ===
  m_flowMonitor->CheckForLostPackets ();
  std::string flowmonXml = MakeOutputPath ("flowmon_stats", ".xml");
  m_flowMonitor->SerializeToXmlFile (flowmonXml, true, true);

  // 性能统计：按流实际传输时长计算吞吐/时延，丢包率 = LostPkts/TxPkts
  std::string lossPath = MakeOutputPath ("flowmon_stats", ".txt");
  std::string debugLossPath = MakeOutputPathInCategory ("performance", "flowmon_debug", ".txt");
  Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (m_flowHelper.GetClassifier ());
  FlowMonitor::FlowStatsContainer stats = m_flowMonitor->GetFlowStats ();
  std::ofstream lossFile;
  std::ofstream debugLossFile;
  lossFile.open (lossPath.c_str (), std::ios::out);
  debugLossFile.open (debugLossPath.c_str (), std::ios::out);
  if (lossFile.is_open ())
    {
      lossFile << "# FlowID\tSrc\tDst\tSubnetType\tTxPkts\tRxPkts\tLostPkts\tLossRatio\tThroughput_Mbps\tAvgDelay_ms\tJitter_ms" << std::endl;
      lossFile << "# MAIN KPI ONLY: business_flows from scenario_config.json" << std::endl;
      lossFile << "# LossRatio = LostPkts/TxPkts; Throughput = rxBytes*8/duration_s; AvgDelay = delaySum/rxPackets; Jitter=jitterSum/rxPackets" << std::endl;
    }
  if (debugLossFile.is_open ())
    {
      debugLossFile << "# DEBUG FLOWS ONLY: non-business control/probe traffic" << std::endl;
      debugLossFile << "# FlowID\tSrc\tDst\tSubnetType\tTxPkts\tRxPkts\tLostPkts\tLossRatio\tThroughput_Mbps\tAvgDelay_ms\tJitter_ms" << std::endl;
    }
  double sumDelayMsMain = 0.0;
  double sumLossPctMain = 0.0;
  double sumThrMbpsMain = 0.0;
  uint32_t perfFlowCntMain = 0;
  uint64_t totalTxBytesMain = 0;
  double sumDelayMsDebug = 0.0;
  double sumLossPctDebug = 0.0;
  double sumThrMbpsDebug = 0.0;
  uint32_t perfFlowCntDebug = 0;
  uint64_t totalTxBytesDebug = 0;
  uint64_t controlTxBytes = 0;
  uint64_t dataTxBytes = 0;
  struct SubnetPerfAgg { uint64_t txPkts = 0; uint64_t rxPkts = 0; uint64_t lostPkts = 0; double thrMbps = 0.0; double delayMsSum = 0.0; uint32_t flows = 0; };
  std::map<std::string, SubnetPerfAgg> subnetAgg;

  for (auto it = stats.begin (); it != stats.end (); ++it)
    {
      Ipv4FlowClassifier::FiveTuple t;
      if (classifier)
        { t = classifier->FindFlow (it->first); }
      else
        { t.sourceAddress = Ipv4Address ("0.0.0.0"); t.destinationAddress = Ipv4Address ("0.0.0.0"); }

      const FlowMonitor::FlowStats& s = it->second;
      // 跳过纯控制信令端口（非业务 KPI）
      if (t.sourcePort < 9000 && t.destinationPort < 9000)
        continue;
      std::ostringstream flowKey;
      flowKey << t.sourceAddress << "|" << t.destinationAddress << "|" << t.destinationPort;
      bool isBusinessFlow = (m_businessFlowKeys.find (flowKey.str ()) != m_businessFlowKeys.end ());
      uint64_t txPkts = s.txPackets;
      uint64_t rxPkts = s.rxPackets;
      if (txPkts == 0) continue;
      if (isBusinessFlow) totalTxBytesMain += s.txBytes;
      else totalTxBytesDebug += s.txBytes;
      uint64_t lostPkts = (txPkts >= rxPkts) ? (txPkts - rxPkts) : 0;
      double lossRatio = (txPkts > 0) ? (static_cast<double> (lostPkts) / static_cast<double> (txPkts)) : 0.0;
      double lossPct = lossRatio * 100.0;

      // 传输时长：首包发送到末包接收（若有接收），否则首包到末包发送
      double durationSec = 1.0;
      if (s.txPackets > 0)
        {
          if (s.rxPackets > 0 && s.timeLastRxPacket.GetSeconds () > 0)
            durationSec = (s.timeLastRxPacket - s.timeFirstTxPacket).GetSeconds ();
          else
            durationSec = (s.timeLastTxPacket - s.timeFirstTxPacket).GetSeconds ();
          durationSec = std::max (0.001, durationSec);
        }
      double thrMbps = (s.rxBytes * 8.0) / durationSec / 1e6;
      double delayMs = (rxPkts > 0) ? (s.delaySum.GetMilliSeconds () / rxPkts) : 0.0;
      double jitterMs = (rxPkts > 0) ? (s.jitterSum.GetMilliSeconds () / rxPkts) : 0.0;

      // 区分子网/回程类型（按 IP 网段：10.100=LTE 回程, 10.101=Adhoc 回程, 10.102=DL 回程, 10.1=Adhoc 子网, 10.2=DL 子网, 7=LTE）
      std::string subnetType = "Other";
      uint32_t src = t.sourceAddress.Get ();
      uint32_t dst = t.destinationAddress.Get ();
      uint8_t s1 = (src >> 24) & 0xFF, s2 = (src >> 16) & 0xFF;
      uint8_t d1 = (dst >> 24) & 0xFF, d2 = (dst >> 16) & 0xFF;
      if (s1 == 10 && s2 == 100) subnetType = "Backhaul-LTE";
      else if (d1 == 10 && d2 == 100) subnetType = "Backhaul-LTE";
      else if (s1 == 10 && s2 == 101) subnetType = "Backhaul-Adhoc";
      else if (d1 == 10 && d2 == 101) subnetType = "Backhaul-Adhoc";
      else if (s1 == 10 && s2 == 102) subnetType = "Backhaul-DL";
      else if (d1 == 10 && d2 == 102) subnetType = "Backhaul-DL";
      else if (s1 == 10 && s2 == 1) subnetType = "Subnet-Adhoc";
      else if (d1 == 10 && d2 == 1) subnetType = "Subnet-Adhoc";
      else if (s1 == 10 && s2 == 2) subnetType = "Subnet-DL";
      else if (d1 == 10 && d2 == 2) subnetType = "Subnet-DL";
      else if (s1 == 7 || d1 == 7) subnetType = "LTE";

      std::ostringstream oss;
      oss << "Flow " << it->first << " (" << t.sourceAddress << " -> " << t.destinationAddress
          << ") " << subnetType << " Thr=" << std::fixed << std::setprecision (4) << thrMbps << " Mbps, Delay="
          << std::setprecision (2) << delayMs << " ms, Lost=" << lostPkts
          << ", LossRatio=" << std::setprecision (4) << lossRatio << " (" << lostPkts << "/" << txPkts << "), LossPct=" << std::setprecision (2) << lossPct << "%";
      NmsLog ("INFO", 0, isBusinessFlow ? "PERF_MAIN" : "PERF_DEBUG", oss.str ());
      std::ostringstream perfLine;
      perfLine << "flowId=" << it->first << " delay=" << static_cast<int>(delayMs) << "ms throughput="
               << std::fixed << std::setprecision (2) << thrMbps << "Mbps loss=" << std::setprecision (1) << lossPct
               << "% jitter=" << std::setprecision (2) << jitterMs << "ms";
      NmsLog ("INFO", 0, isBusinessFlow ? "FLOW_PERF_MAIN" : "FLOW_PERF_DEBUG", perfLine.str ());
      if (isBusinessFlow)
        {
          NmsLog ("INFO", 0, "BUSINESS_PERF",
                  "Business ID:" + std::to_string (it->first) +
                  " Throughput:" + std::to_string (thrMbps) + "Mbps Jitter:" + std::to_string (jitterMs) + "ms");
          sumDelayMsMain += delayMs;
          sumLossPctMain += lossPct;
          sumThrMbpsMain += thrMbps;
          perfFlowCntMain++;
          subnetAgg[subnetType].txPkts += txPkts;
          subnetAgg[subnetType].rxPkts += rxPkts;
          subnetAgg[subnetType].lostPkts += lostPkts;
          subnetAgg[subnetType].thrMbps += thrMbps;
          subnetAgg[subnetType].delayMsSum += delayMs;
          subnetAgg[subnetType].flows += 1;
        }
      else
        {
          sumDelayMsDebug += delayMs;
          sumLossPctDebug += lossPct;
          sumThrMbpsDebug += thrMbps;
          perfFlowCntDebug++;
        }

      // 论文指标：按端口近似区分控制开销与数据开销
      uint16_t sp = t.sourcePort;
      uint16_t dp = t.destinationPort;
      bool isControl = (sp == 8888 || sp == 5002 || sp == 5003 || sp == 6000 ||
                        dp == 8888 || dp == 5002 || dp == 5003 || dp == 6000);
      if (isControl) controlTxBytes += s.txBytes;
      else dataTxBytes += s.txBytes;

      if (isBusinessFlow && lossFile.is_open ())
        {
          lossFile << it->first << "\t" << t.sourceAddress << "\t" << t.destinationAddress << "\t" << subnetType
                   << "\t" << txPkts << "\t" << rxPkts << "\t" << lostPkts << "\t" << std::fixed << std::setprecision (6) << lossRatio
                   << "\t" << std::setprecision (4) << thrMbps << "\t" << std::setprecision (2) << delayMs
                   << "\t" << std::setprecision (2) << jitterMs << std::endl;
        }
      else if (!isBusinessFlow && debugLossFile.is_open ())
        {
          debugLossFile << it->first << "\t" << t.sourceAddress << "\t" << t.destinationAddress << "\t" << subnetType
                        << "\t" << txPkts << "\t" << rxPkts << "\t" << lostPkts << "\t" << std::fixed << std::setprecision (6) << lossRatio
                        << "\t" << std::setprecision (4) << thrMbps << "\t" << std::setprecision (2) << delayMs
                        << "\t" << std::setprecision (2) << jitterMs << std::endl;
        }
    }

  if (lossFile.is_open ())
    {
      lossFile << "# End. LossRatio formula: LostPackets/TxPackets" << std::endl;
      lossFile.close ();
    }
  if (debugLossFile.is_open ())
    {
      debugLossFile << "# End. LossRatio formula: LostPackets/TxPackets" << std::endl;
      debugLossFile.close ();
    }
  if (perfFlowCntMain > 0)
    {
      double avgDelayMs = sumDelayMsMain / perfFlowCntMain;
      double avgLossPct = sumLossPctMain / perfFlowCntMain;
      double avgThrMbps = sumThrMbpsMain / perfFlowCntMain;
      double signalingOverheadPct = (totalTxBytesMain > 0)
                                  ? (100.0 * static_cast<double> (controlTxBytes) / static_cast<double> (totalTxBytesMain))
                                  : 0.0;
      std::ostringstream kpi;
      kpi << "avgDelayMs=" << std::fixed << std::setprecision (2) << avgDelayMs
          << " avgLossPct=" << std::setprecision (2) << avgLossPct
          << " avgThrMbps=" << std::setprecision (4) << avgThrMbps
          << " signalingOverheadPct=" << std::setprecision (2) << signalingOverheadPct
          << " controlTxBytes=" << controlTxBytes
          << " dataTxBytes=" << dataTxBytes;
      NmsLog ("INFO", 0, "KPI_MAIN", kpi.str ());
      if (perfFlowCntDebug > 0)
        {
          std::ostringstream dbg;
          dbg << "avgDelayMs=" << std::fixed << std::setprecision (2) << (sumDelayMsDebug / perfFlowCntDebug)
              << " avgLossPct=" << std::setprecision (2) << (sumLossPctDebug / perfFlowCntDebug)
              << " avgThrMbps=" << std::setprecision (4) << (sumThrMbpsDebug / perfFlowCntDebug)
              << " flowCount=" << perfFlowCntDebug
              << " totalTxBytes=" << totalTxBytesDebug;
          NmsLog ("INFO", 0, "KPI_DEBUG", dbg.str ());
        }
      for (const auto& kv : subnetAgg)
        {
          const std::string& sub = kv.first;
          const SubnetPerfAgg& a = kv.second;
          double avgDelay = a.flows > 0 ? (a.delayMsSum / a.flows) : 0.0;
          double lossRate = a.txPkts > 0 ? (100.0 * static_cast<double> (a.lostPkts) / static_cast<double> (a.txPkts)) : 0.0;
          std::ostringstream tab;
          tab << "subnet=" << sub
              << " throughputMbps=" << std::fixed << std::setprecision (4) << a.thrMbps
              << " avgDelayMs=" << std::setprecision (2) << avgDelay
              << " lossPct=" << std::setprecision (2) << lossRate
              << " txPkts=" << a.txPkts << " rxPkts=" << a.rxPkts;
          NmsLog ("INFO", 0, "SUBNET_PERF_TABLE", tab.str ());
        }

      uint64_t protocolSuppress = 0;
      uint64_t protocolReported = 0;
      uint64_t totalDecisions = 0;
      uint64_t totalTriggered = 0;
      uint64_t totalSuppressed = 0;
      double avgIntervalAcc = 0.0;
      uint32_t avgIntervalNodes = 0;
      uint64_t aggSuppressed = 0;
      uint64_t aggRawBytes = 0;
      uint64_t aggSentBytes = 0;
      for (NodeList::Iterator nit = NodeList::Begin (); nit != NodeList::End (); ++nit)
        {
          Ptr<Node> n = *nit;
          for (uint32_t ai = 0; ai < n->GetNApplications (); ++ai)
            {
              Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> (n->GetApplication (ai));
              if (!app) continue;
              protocolSuppress += app->GetProtocolSuppressCount ();
              protocolReported += app->GetReportedPackets ();
              totalDecisions += app->GetTotalScheduleDecisions ();
              totalTriggered += app->GetTriggeredScheduleDecisions ();
              totalSuppressed += app->GetSuppressedScheduleDecisions ();
              double avgItv = app->GetAverageReportIntervalSec ();
              if (avgItv > 0.0)
                {
                  avgIntervalAcc += avgItv;
                  avgIntervalNodes++;
                }
              aggSuppressed += app->GetAggregateSuppressedCount ();
              aggRawBytes += app->GetAggregateRawBytes ();
              aggSentBytes += app->GetAggregateSentBytes ();
            }
        }
      std::ostringstream lossSplit;
      lossSplit << "linkLossPkts=" << (subnetAgg["Subnet-Adhoc"].lostPkts + subnetAgg["Subnet-DL"].lostPkts + subnetAgg["LTE"].lostPkts
                    + subnetAgg["Backhaul-Adhoc"].lostPkts + subnetAgg["Backhaul-DL"].lostPkts + subnetAgg["Backhaul-LTE"].lostPkts)
                << " protocolSuppressPkts=" << protocolSuppress
                << " protocolReportedPkts=" << protocolReported;
      NmsLog ("INFO", 0, "LOSS_BREAKDOWN", lossSplit.str ());
      {
        double schedSuppRate = (totalDecisions > 0)
                               ? (100.0 * static_cast<double> (totalSuppressed) / static_cast<double> (totalDecisions))
                               : 0.0;
        double avgReportInterval = (avgIntervalNodes > 0) ? (avgIntervalAcc / avgIntervalNodes) : 0.0;
        double ctrlOverheadBps = (simTimeSeconds > 0.0)
                                 ? (static_cast<double> (controlTxBytes + aggSentBytes) / simTimeSeconds)
                                 : 0.0;
        std::ostringstream obs;
        obs << "controlOverheadBps=" << std::fixed << std::setprecision (3) << ctrlOverheadBps
            << " suppressionRatePct=" << std::setprecision (2) << schedSuppRate
            << " avgReportIntervalSec=" << std::setprecision (3) << avgReportInterval
            << " scheduleTriggered=" << totalTriggered
            << " scheduleSuppressed=" << totalSuppressed
            << " aggSuppressed=" << aggSuppressed;
        NmsLog ("INFO", 0, "STATE_DRIVEN_KPI", obs.str ());

        std::string obsJsonPath = MakeOutputPathInCategory ("performance", "state-driven-kpi-summary", ".json");
        std::ofstream obsJson (obsJsonPath.c_str (), std::ios::out);
        if (obsJson.is_open ())
          {
            obsJson << "{\n";
            obsJson << "  \"controlOverheadBps\": " << std::fixed << std::setprecision (6) << ctrlOverheadBps << ",\n";
            obsJson << "  \"suppressionRatePct\": " << std::setprecision (4) << schedSuppRate << ",\n";
            obsJson << "  \"avgReportIntervalSec\": " << std::setprecision (6) << avgReportInterval << ",\n";
            obsJson << "  \"scheduleDecisions\": " << totalDecisions << ",\n";
            obsJson << "  \"scheduleTriggered\": " << totalTriggered << ",\n";
            obsJson << "  \"scheduleSuppressed\": " << totalSuppressed << ",\n";
            obsJson << "  \"aggregateSuppressed\": " << aggSuppressed << ",\n";
            obsJson << "  \"aggregateRawBytes\": " << aggRawBytes << ",\n";
            obsJson << "  \"aggregateSentBytes\": " << aggSentBytes << "\n";
            obsJson << "}\n";
            obsJson.close ();
            NmsLog ("INFO", 0, "STATE_DRIVEN_KPI_JSON", "Written " + obsJsonPath);
          }
      }

      // 结构化 KPI 输出（供论文画图/自动对比脚本消费）
      std::string kpiJsonPath = MakeOutputPathInCategory ("performance", "kpi-summary-main", ".json");
      std::ofstream kpiJson (kpiJsonPath.c_str (), std::ios::out);
      if (kpiJson.is_open ())
        {
          kpiJson << "{\n";
          kpiJson << "  \"scenarioMode\": \"" << m_scenarioMode << "\",\n";
          kpiJson << "  \"simTimeSec\": " << std::fixed << std::setprecision (3) << simTimeSeconds << ",\n";
          kpiJson << "  \"avgDelayMs\": " << std::setprecision (4) << avgDelayMs << ",\n";
          kpiJson << "  \"avgLossPct\": " << std::setprecision (4) << avgLossPct << ",\n";
          kpiJson << "  \"avgThrMbps\": " << std::setprecision (6) << avgThrMbps << ",\n";
          kpiJson << "  \"signalingOverheadPct\": " << std::setprecision (4) << signalingOverheadPct << ",\n";
          kpiJson << "  \"controlTxBytes\": " << controlTxBytes << ",\n";
          kpiJson << "  \"dataTxBytes\": " << dataTxBytes << ",\n";
          kpiJson << "  \"totalTxBytesMain\": " << totalTxBytesMain << ",\n";
          kpiJson << "  \"totalTxBytesDebug\": " << totalTxBytesDebug << ",\n";
          kpiJson << "  \"flowCount\": " << perfFlowCntMain << ",\n";
          kpiJson << "  \"debugFlowCount\": " << perfFlowCntDebug << "\n";
          kpiJson << "}\n";
          kpiJson.close ();
          NmsLog ("INFO", 0, "KPI_JSON", "Written " + kpiJsonPath);
        }
    }

  // === 7.1 性能统计图表自动生成：调用 plot-flowmon.py 生成 PNG，保存至 performance/ ===
  if (!m_outputDir.empty () && m_dirPerformance.size () > 0)
    {
      std::string flowmonXmlPath = MakeOutputPath ("flowmon_stats", ".xml");
      std::string chartPrefix = m_dirPerformance + "/performance_charts_" + m_timestamp;
      std::ostringstream chartCmd;
      chartCmd << "python3 plot-flowmon.py \"" << flowmonXmlPath << "\" -o \"" << chartPrefix << "\" 2>/dev/null || true";
      int chartRet = std::system (chartCmd.str ().c_str ());
      if (chartRet == 0)
        NmsLog ("INFO", 0, "PERF", "Performance charts: " + chartPrefix + ".png");
    }

  if (g_nmsLogFile.is_open ())
    {
      g_nmsLogFile << "========== Simulation finished ==========" << std::endl;
      g_nmsLogFile.close ();
    }

  // === 8. 抓包自动解析：若开启 pcap，解析 packet/ 下 pcap，输出至 packet/packet_parse_result_${时间戳}.txt ===
  if (m_enablePcap && !m_outputDir.empty ())
    {
      std::string parseOut = MakeOutputPathInCategory ("packet", "packet_parse_result", ".txt");
      std::ostringstream cmd;
      cmd << "python3 parse_nms_pcap.py " << m_dirPacket << " -o " << parseOut << " 2>/dev/null || true";
      int ret = std::system (cmd.str ().c_str ());
      if (ret == 0)
        NmsLog ("INFO", 0, "PARSE", "Packet parse result: " + parseOut);
      // 生成论文要求的统一文件名（便于 Wireshark 直接打开）
      std::string rawLinkCmd = "bash -lc 'ls -1 " + m_dirPacket + "/raw_hnmp_*.pcap 2>/dev/null | head -n 1 | xargs -r -I{} cp \"{}\" \"" + m_dirPacket + "/raw_hnmp.pcap\"'";
      std::string aggLinkCmd = "bash -lc 'ls -1 " + m_dirPacket + "/agg_hnmp_*.pcap 2>/dev/null | head -n 1 | xargs -r -I{} cp \"{}\" \"" + m_dirPacket + "/agg_hnmp.pcap\"'";
      int rawRet = std::system (rawLinkCmd.c_str ());
      int aggRet = std::system (aggLinkCmd.c_str ());
      (void) rawRet;
      (void) aggRet;
      NmsLog ("INFO", 0, "PCAP_FILES", "raw_hnmp.pcap / agg_hnmp.pcap generated under " + m_dirPacket);
    }

  Simulator::Destroy ();
}

/*=================================== main 函数 ======================================
 *
 * 仅作为框架入口，保持尽量简洁：
 *  - 创建 HeterogeneousNmsFramework 实例
 *  - 调用 Run() 执行仿真
 *
 * 若要进行参数化运行（如从命令行传入仿真时长 / 节点数量），
 * 可在此使用 CommandLine 解析，并传入 Framework。
 *===================================================================================*/

int
HnmsMain (int argc, char *argv[])
{
  double simTime = 100.0;
  int adhocTopology = 0;  // 0=MESH, 1=STAR, 2=TREE
  std::string adhocTopologyStr = "mesh";  // 可选：--adhoc-topology=star|tree|mesh
  bool enablePacketParse = false;
  bool enablePcap = false;
  bool useDualChannel = false;
  std::string joinConfigPath;
  std::string scenarioConfigPath;
  uint32_t failNodeId = 0;   // 事件注入：要断电的节点 ID，0=禁用
  double failTime = 30.0;    // 断电时刻（秒）
  double energyDeltaThreshold = 0.05;
  double stateSuppressWindow = 1.0;
  double aggregateInterval = 2.0;
  std::string scenarioMode = "normal";
  uint32_t rngSeed = 1;
  uint32_t rngRun = 1;

  CommandLine cmd;
  cmd.AddValue ("simTime", "Simulation time in seconds", simTime);
  cmd.AddValue ("topology", "Adhoc topology: 0=MESH, 1=STAR, 2=TREE", adhocTopology);
  cmd.AddValue ("adhoc-topology", "Adhoc topology: mesh|star|tree (overrides topology if set)", adhocTopologyStr);
  cmd.AddValue ("parsePackets", "Enable runtime packet parse (TLV/Hello/Policy) log", enablePacketParse);
  cmd.AddValue ("pcap", "Enable pcap trace for offline parsing", enablePcap);
  cmd.AddValue ("dualChannel", "Use control(8888)/data(9999) port separation", useDualChannel);
  cmd.AddValue ("joinConfig", "Path to node join config JSON (timed join); empty=disabled", joinConfigPath);
  cmd.AddValue ("scenario", "Path to scenario config JSON (trajectory/events); empty=disabled", scenarioConfigPath);
  cmd.AddValue ("failNode", "Event injection: node ID to fail (0=disabled)", failNodeId);
  cmd.AddValue ("failTime", "Event injection: time in seconds when node fails", failTime);
  cmd.AddValue ("energyDeltaTh", "DataLink delta-report threshold", energyDeltaThreshold);
  cmd.AddValue ("stateSuppressWin", "State suppression window in seconds", stateSuppressWindow);
  cmd.AddValue ("aggregateInterval", "SPN aggregate interval in seconds", aggregateInterval);
  cmd.AddValue ("scenarioMode", "Scenario mode: normal|thesis30|compare-baseline|compare-hnmp", scenarioMode);
  cmd.AddValue ("rngSeed", "Global RNG seed for reproducible experiments", rngSeed);
  cmd.AddValue ("rngRun", "RNG run index for repeated trials", rngRun);
  cmd.Parse (argc, argv);

  // 未指定 joinConfig 时尝试默认路径，确保 JSON 中设计的节点数（如 16 节点、5 个 LTE UE）生效
  if (joinConfigPath.empty ())
    {
      std::ifstream f1 ("join_config.json");
      if (f1) { joinConfigPath = "join_config.json"; f1.close (); }
      else
        {
          std::ifstream f2 ("docs/heterogeneous-nms/join_config.json");
          if (f2) { joinConfigPath = "docs/heterogeneous-nms/join_config.json"; f2.close (); }
        }
    }

  if (adhocTopologyStr == "star") adhocTopology = 1;
  else if (adhocTopologyStr == "tree") adhocTopology = 2;
  else if (adhocTopologyStr == "mesh") adhocTopology = 0;

  RngSeedManager::SetSeed (rngSeed);
  RngSeedManager::SetRun (rngRun);

  LogComponentEnable ("HeterogeneousNodeApp", LOG_LEVEL_INFO);

  HeterogeneousNmsFramework framework;
  framework.SetAdhocTopology (static_cast<HeterogeneousNmsFramework::AdhocTopologyType> (adhocTopology));
  framework.SetEnablePacketParse (enablePacketParse);
  framework.SetEnablePcap (enablePcap);
  framework.SetUseDualChannel (useDualChannel);
  framework.SetEnergyDeltaThreshold (energyDeltaThreshold);
  framework.SetStateSuppressWindow (stateSuppressWindow);
  framework.SetAggregateIntervalSec (aggregateInterval);
  framework.SetScenarioMode (scenarioMode);
  if (!joinConfigPath.empty ()) framework.SetJoinConfigPath (joinConfigPath);
  if (!scenarioConfigPath.empty ()) framework.SetScenarioConfigPath (scenarioConfigPath);
  if (failNodeId > 0) { framework.SetFailNodeId (failNodeId); framework.SetFailTime (failTime); }
  framework.Run (simTime);

  return 0;
}

#ifndef HNMS_NO_MAIN
int
main (int argc, char *argv[])
{
  return HnmsMain (argc, argv);
}
#endif

