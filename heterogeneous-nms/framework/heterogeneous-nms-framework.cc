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
#include "ns3/traffic-control-module.h"
#include "ns3/traffic-control-layer.h"
#include "ns3/applications-module.h"
#include "ns3/onoff-application.h"
#include "ns3/node-list.h"
#include "ns3/propagation-loss-model.h"

// LTE / EPC 模块
#include "ns3/lte-module.h"
#include "ns3/point-to-point-epc-helper.h"

#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <limits>
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
#include "../core/config_loader.h"
#include "../core/event_scheduler.h"
#include "../core/structured_log.h"
#include "../core/state_manager.h"
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
  double staticComputeCapability; ///< 静态算力能力 [0,1]，<0 表示不覆盖
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
      c.initialRateMbps = c.initialEnergy = c.initialEnergyMah = c.initialLinkQuality = c.staticComputeCapability = -1.0;
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
      size_t pComp = skipToVal ("\"static_compute_capability\"");
      if (pComp != std::string::npos) c.staticComputeCapability = getNum (pComp);
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
      if (c.staticComputeCapability >= 0.0 &&
          (c.staticComputeCapability <= 0.0 || c.staticComputeCapability > 1.0))
        return "JoinConfig: static_compute_capability must be (0,1] node_id=" + std::to_string (c.nodeId);
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
                     const std::string& role = "", const std::string& extraFields = "")
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
  if (!extraFields.empty ())
    g_jsonlStateFile << extraFields;
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

/// 解析 scenario events 对象中的数值字段（键存在时写入 out）
static bool
ParseJsonNumberIfKey (const std::string& obj, const char* key, double& out)
{
  std::string pat = std::string ("\"") + key + "\"";
  size_t p = obj.find (pat);
  if (p == std::string::npos)
    {
      return false;
    }
  size_t colon = obj.find (':', p);
  if (colon == std::string::npos)
    {
      return false;
    }
  out = std::strtod (obj.c_str () + colon + 1, nullptr);
  return true;
}

/// 解析 scenario events 对象中的字符串字段
static bool
ParseJsonStringIfKey (const std::string& obj, const char* key, std::string& out)
{
  std::string pat = std::string ("\"") + key + "\"";
  size_t p = obj.find (pat);
  if (p == std::string::npos)
    {
      return false;
    }
  size_t colon = obj.find (':', p);
  if (colon == std::string::npos)
    {
      return false;
    }
  size_t q1 = obj.find ('"', colon + 1);
  if (q1 == std::string::npos)
    {
      return false;
    }
  size_t q2 = obj.find ('"', q1 + 1);
  if (q2 == std::string::npos)
    {
      return false;
    }
  out = obj.substr (q1 + 1, q2 - q1 - 1);
  return true;
}

// ====================== 场景想定模块（扩展接口） ======================
struct TrajectoryPoint { double t; double x, y, z; };
/// 事件定义：由 scenario_config.json 的 events 数组解析
struct ScenarioEvent
{
  double time;        ///< 触发时刻（秒）；NODE_OFFLINE/NODE_FAIL 用；物理注入可与 inject_time 对齐
  double injectTime;  ///< 注入时刻（秒），<0 表示未配置（回退用 time）
  std::string type;   ///< 如 "NODE_FAIL"
  uint32_t target;    ///< 目标节点 ID（如 NODE_FAIL 时）
  uint32_t triggerNodeId; ///< 触发节点 ID（SPN_SWITCH）
  uint32_t newSpnNodeId;  ///< 新主 SPN 节点 ID（SPN_SWITCH）
  std::string offlineReason; ///< NODE_OFFLINE 等：reason 字段
  double injectedEnergy;       ///< NODE_ENERGY_FAIL
  double injectedLinkQuality;  ///< LINK_INTERFERENCE_FAIL
  double threshold;            ///< 监测阈值（依 type 解释）
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
  double spnElectionTimeoutSec{0.0};       ///< 根级 spn_election_timeout（可选）
  uint32_t spnHeartbeatMissThreshold{0}; ///< 根级 spn_heartbeat_miss_threshold（可选）
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
  {
    double tmpD = 0.0;
    if (ParseJsonNumberIfKey (content, "spn_election_timeout", tmpD))
      {
        out.spnElectionTimeoutSec = tmpD;
      }
    if (ParseJsonNumberIfKey (content, "spn_heartbeat_miss_threshold", tmpD))
      {
        out.spnHeartbeatMissThreshold = static_cast<uint32_t> (tmpD + 0.5);
      }
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
              ev.injectTime = -1.0;
              ev.injectedEnergy = -1.0;
              ev.injectedLinkQuality = -1.0;
              ev.threshold = -1.0;
              {
                double tmpD = 0.0;
                ParseJsonStringIfKey (obj, "reason", ev.offlineReason);
                if (ParseJsonNumberIfKey (obj, "inject_time", tmpD))
                  {
                    ev.injectTime = tmpD;
                  }
                if (ParseJsonNumberIfKey (obj, "injected_energy", tmpD))
                  {
                    ev.injectedEnergy = tmpD;
                  }
                if (ParseJsonNumberIfKey (obj, "injected_link_quality", tmpD))
                  {
                    ev.injectedLinkQuality = tmpD;
                  }
                if (ParseJsonNumberIfKey (obj, "threshold", tmpD))
                  {
                    ev.threshold = tmpD;
                  }
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
                     const std::string& role = "", const std::string& extraFields = "")
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
  if (!extraFields.empty ()) g_jsonlStateFile << extraFields;
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
  /// 仅 Adhoc：簇首（与 SPN 正交，可同时为 Primary/Backup SPN）
  struct AdhocChScoreSnapshot
  {
    double total {0.0};
    double energy {0.0};
    double degree {0.0};
    double mobility {0.0};
    double centrality {0.0};
  };
  bool IsClusterHead () const;
  AdhocChScoreSnapshot GetAdhocChScoreSnapshot () const;
  uint32_t GetOneHopNeighborCount () const;
  /// 供 JSONL 全量快照：应用是否在运行、子网类型与 UV-MIB 瞬时值（不修改选举状态）
  bool IsApplicationRunning () const { return m_running; }
  SubnetType GetSubnetType () const { return m_subnetType; }
  double GetUvMibEnergy () const { return m_uvMib.m_energy; }
  double GetUvMibLinkQuality () const { return m_uvMib.m_linkQuality; }
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
  /// 场景事件：在 inject_time 强制写入能量/链路质量（不触发退网）
  void ApplyScenarioInjectEnergy (double energy);
  void ApplyScenarioInjectLinkQuality (double linkQuality);
  /// LTE：区分 eNB / UE 以采用不同 Score 权重（仅 SUBNET_LTE 有效）
  void SetLteNodeKind (bool isEnb);

  /// 时序入网：设置本节点入网时间（秒），未入网前应用层静默、不耗电
  void SetJoinTime (double joinTimeSec);
  /// 论文 4.5.2/4.6：上报抑制与聚合参数
  void SetProtocolTunables (double energyDeltaThreshold, double suppressWindowSec, double aggregateIntervalSec);
  void SetSpnElectionTunables (double electionTimeoutSec, uint8_t heartbeatMissThreshold, double deltaTh = -1.0);
  void SetStaticComputeCapability (double capability);

  /// 事件注入：强制节点失效（能量置 0、停止发包、关闭 socket），用于模拟断电/宕机
  void ForceFail ();
  /// 事件注入：触发一次立即重选（用于节点故障后加速收敛）
  void TriggerElectionNow (const std::string& electReason = std::string ());
  /// 退网/拓扑剔除：从本地与共享 Score 视图移除某节点，避免重选仍计入已失效节点
  void SpnForgetPeer (uint32_t peerNodeId);

  /// 每次仿真 Run 前清空静态子网选举状态（避免同进程多次 Run 残留）
  static void ResetSharedElectionState ();
  /// 子网内 join_time<=0 的节点数，用于首轮选举等待分数视图收敛
  static void SetExpectedInitialElectionMembers (SubnetType st, uint32_t count);

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
  struct WeightVector
  {
    double wEnergy;
    double wTopo;
    double wLink;
    double wComp;
  };
  struct SpnCandidateInfo
  {
    uint32_t nodeId {0};
    SubnetType subnetType {SUBNET_ADHOC};
    double L_energy {0.0};
    double L_mobility {0.0};
    double L_struct {0.0};
    double L_topo {0.0};
    double L_link {0.0};
    double L_comp_static {1.0};
    double rho_queue {0.0};
    double L_comp {0.0};
    double totalScore {0.0};
    bool disqualified {false};
  };
  double ComputeLmobility (double speedMps) const;
  double ComputeLstruct (uint32_t nodeId,
                         SubnetType subnetType,
                         const std::vector<uint32_t>& oneHopNeighbors,
                         const std::vector<uint32_t>& twoHopNeighbors,
                         uint32_t clusterSize) const;
  double ComputeLtopo (uint32_t nodeId,
                       SubnetType subnetType,
                       double speedMps,
                       const std::vector<uint32_t>& oneHopNeighbors,
                       const std::vector<uint32_t>& twoHopNeighbors,
                       uint32_t clusterSize) const;
  double ComputeLlink (uint32_t nodeId,
                       const std::map<uint32_t, double>& neighborSinrMap) const;
  double ComputeLcomp (double staticCapability, double queueOccupancy, bool* disqualified) const;
  double ComputeSpnScore (const SpnCandidateInfo& info, SubnetType subnetType) const;
  std::vector<uint32_t> GetOneHopNeighbors () const;
  std::vector<uint32_t> GetTwoHopNeighborsProxy () const;
  uint32_t GetSubnetClusterSize () const;
  std::map<uint32_t, double> BuildNeighborSinrProxyMap () const;

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

  /// 仅同步分数到共享视图并刷新本地角色（不触发主备重算、不产生 SPN_ELECT）
  void SpnMergeScoresIntoShared (double myScore);
  void ApplyLocalRoleFromShared ();
  /// 与 RunFullElection 内逻辑一致：当仅从共享视图更新角色时（例如他节点触发选举），必须重绑心跳定时器
  void ResyncSpnHeartbeatTimerFromRoleTransition (bool oldIsSpn, bool oldBackup);
  void SpnSyncScoresOnly (double myScore);
  /// 条件触发完整选举：首轮 / 新节点入网 / 主 SPN 持续劣化（非 STATE_CHANGE 路径）
  void MaybeTriggerSpnElection (double myScore);
  /// 完整子网选举（主备重算、可产生 SPN_ELECT/SPN_ANNOUNCE）；故障与心跳也走此路径
  void RunFullElection (const std::string& electReason = std::string ());
  /// 条件 C：仅 Primary 上周期评估（10s 一次）
  void EvaluateConditionC ();
  void OnConditionCEval ();
  void SyncConditionCTimer ();
  /// Adhoc 簇首：全局选举（静态）、周期评估（仅 Primary 上定时器）
  static void RunClusterHeadElectionGlobal ();
  void OnAdhocChPeriodicEval ();
  void SyncAdhocChPeriodicTimer ();
  /// 延迟一次 Score 泛洪（使用当前计算的分数），用于入网后加速全子网分数对齐
  void BroadcastScoreFloodCurrent ();
  /// StartApplication 中延迟 0.1s 触发首轮选举（reason=initial_lock，与 LTE 宣告风格一致）
  void RunInitialLockElection (void);
  /// 选举状态变更后由当选 Primary 节点输出 SPN_ANNOUNCE（当 RunFullElection 调用者非 Primary 时由调度执行）
  void PublishSpnAnnounceAfterElection (double primaryScoreVal, double backupScoreVal, const std::string& changeReason);
  static Ptr<HeterogeneousNodeApp> FindHetAppOnNode (uint32_t nodeId, SubnetType st);

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
  EventId         m_conditionCTimer;      ///< 条件 C：仅 Primary 上 10s 周期评估
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
  static const double SPN_SWITCH_HYSTERESIS;       ///< SPN 切换迟滞阈值（旧代理逻辑）
  static const double V_TH;
  static const double LAMBDA;
  static const double ALPHA;
  static const double BETA;
  static const double GAMMA;
  static const double SINR_MIN;
  static const double SINR_MAX;
  static const double DELTA_TH;
  static const std::map<SubnetType, WeightVector> SUBNET_WEIGHTS;
  /// Score 平滑：对瞬时 UV-MIB/拓扑输入与最终得分做 EMA，抑制随机扰动导致的选举抖动
  static const double SCORE_INPUT_EMA_ALPHA;       ///< 能量/链路/拓扑度量 EMA 系数（越大越跟瞬时）
  static const double SCORE_OUTPUT_EMA_ALPHA;      ///< 最终 Score EMA 系数
  static const double SPN_PRIMARY_SCORE_THRESHOLD; ///< 主 SPN 得分阈值（低于该值才触发一次得分降级切换）
  static const double SPN_INITIAL_ELECTION_WARMUP_SEC; ///< 初始选举预热窗口，避免启动瞬时误选
  static const double SPN_INITIAL_ELECTION_DEADLINE_SEC; ///< 首轮选举最长等待（秒），超时则用已有分数完成选举
  static const uint32_t SPN_PRIMARY_STABLE_TICKS;  ///< 连续多少次 RunElection 认定同一主 SPN 后才切换 committed 主
  /// 选举与分数更新解耦：重选最小间隔（秒，仅用于条件 C 触发完整重选的门控）
  static constexpr double MIN_ELECT_INTERVAL_SEC = 15.0;
  /// 仅条件 C：非 Primary 分数需持续高于 Primary + Margin 才进入挑战计数（条件 A 迟入网不使用 Margin）
  static constexpr double SCORE_ELECT_MARGIN = 0.1;
  static constexpr double CONDITION_C_PERIOD_SEC = 10.0;
  static constexpr double CH_EVAL_PERIOD_SEC = 15.0;
  static constexpr double CH_PERIODIC_SCORE_MARGIN = 0.15;
  static constexpr uint32_t CH_PERIODIC_CONFIRM_PERIODS = 2u;
  static constexpr uint32_t CONDITION_C_CONFIRM_PERIODS = 2;
  uint32_t        m_currentPrimaryId;      ///< 当前感知的主SPN
  double          m_lastPrimaryHeartbeatTs;///< 最近收到主SPN心跳时刻
  uint8_t         m_missedHeartbeats;      ///< 连续丢心跳计数
  uint32_t        m_heartbeatSendSeq;      ///< 主 SPN 心跳发送序号（验证日志）
  uint32_t        m_lastSyncedCommittedPrimaryId; ///< 上次从共享状态同步的主 SPN（用于主切换时重置备用心跳跟踪）
  double          m_backupHeartbeatGraceUntilTs; ///< 备 SPN：主切换后到此时刻前不累计心跳丢失（等待新主首包）
  bool            m_failoverPending;       ///< 已触发接管判定，等待成为主SPN
  double          m_failoverStartTs;       ///< 触发接管时刻，用于自愈耗时统计
  double          m_joinTime;             ///< 入网时间（秒），未到则应用层静默、不耗电
  bool            m_scoreFilterInit;       ///< EMA 状态是否已初始化
  double          m_initialEnergyRef;      ///< 初始能量（用于 E_norm=当前/初始；与 UV-MIB 同量纲，通常为 0~1）
  bool            m_lteIsEnb;              ///< LTE eNB 为 true，UE 为 false（仅 SUBNET_LTE）
  double          m_emaEnergy;             ///< 保留：历史 EMA 字段（兼容/预留）
  double          m_emaLink;               ///< 保留
  double          m_emaTopoMetric;         ///< 保留
  double          m_velocityEma;           ///< 保留
  double          m_emaFinalScore;           ///< 对外广播/写入 global 的最终平滑 Score（输出 EMA，约束 [0,1]）
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
  bool            m_initialScoreFloodSent; ///< 尽快广播一次 Score 泛洪，使子网分数视图一致
  /// join_time>0 的节点：首次跨过入网时刻后是否已做过「新节点 vs Primary」重选评估
  bool            m_postJoinElectEvaluated;
  /// Adhoc/DataLink 最近一次 SPN 三分项归一化（用于日志与可视化）
  double          m_lastSpnBatteryNorm;
  double          m_lastSpnConnNorm;
  double          m_lastSpnStabNorm;
  double          m_staticComputeCapability; ///< 静态算力能力 [0,1]
  double          m_queueOccupancyRatio;     ///< 发送队列占用率 [0,1]（轻量监控）
  double          m_spnDeltaThreshold;       ///< SPN 切换迟滞阈值（可配置）
  struct SpnElectHistoryEntry
  {
    double timeSec;
    uint32_t primaryId;
    uint32_t backupId;
    std::string reason;
  };
  struct SharedSpnState
  {
    bool initialized;
    uint32_t primaryId;
    uint32_t backupId;
    bool failoverUsed;
    bool energySwitchUsed;
    bool frozen;
    double initializedTs;
    double lastElectTs;           ///< 最近一次完成完整选举计算的时刻（秒），<0 表示尚未记录
    bool stablePhase;             ///< 已完成首轮 INIT 之后为 true（STABLE_PHASE）
    uint32_t challengeChallengerId; ///< 条件 C：当前挑战者 nodeId（0 表示无）
    uint32_t challengeConsecutive;  ///< 条件 C：连续满足周期数，达到 CONDITION_C_CONFIRM_PERIODS 触发重选
    std::vector<SpnElectHistoryEntry> electHistory;
    std::map<uint32_t, double> scoreByNode;
    std::map<uint32_t, double> scoreTimeByNode;
    SharedSpnState ()
      : initialized (false),
        primaryId (0),
        backupId (0),
        failoverUsed (false),
        energySwitchUsed (false),
        frozen (false),
        initializedTs (0.0),
        lastElectTs (-1.0),
        stablePhase (false),
        challengeChallengerId (0),
        challengeConsecutive (0)
    {}
  };
  static std::map<uint8_t, SharedSpnState> s_sharedSpnState; ///< 子网维度共享主备状态（Adhoc/DataLink）
  static uint32_t s_expectedAdhocInitialMembers;   ///< join_time<=0 的自组网节点数（首轮选举等待用）
  static uint32_t s_expectedDatalinkInitialMembers; ///< join_time<=0 的数据链节点数
  static bool s_globalFailoverUsed; ///< 兼容旧日志/统计（不再阻塞重选）
  static bool s_globalEnergyUsed;   ///< 兼容旧日志/统计（不再阻塞重选）
  struct ChScoreBreakdownInternal
  {
    double total {0.0};
    double energy {0.0};
    double degree {0.0};
    double mobility {0.0};
    double centrality {0.0};
  };
  struct AdhocChElectHistoryEntry
  {
    double timeSec {0.0};
    uint32_t clusterHeadId {0};
    std::string reason;
  };
  struct SharedAdhocChState
  {
    uint32_t clusterHeadId {0};
    std::vector<AdhocChElectHistoryEntry> electHistory;
    uint32_t challengerId {0};
    uint32_t challengerConsecutive {0};
  };
  static SharedAdhocChState s_adhocChState;
  static std::string s_pendingChElectReason;
  static EventId s_deferredChElectEvent;
  bool m_isClusterHead;
  ChScoreBreakdownInternal m_chScoreSaved;
  EventId m_chPeriodicEvent;
  static void ComputeAdhocChScoresMap (std::map<uint32_t, ChScoreBreakdownInternal>& byId,
                                       uint32_t& bestId, double& bestTotal);
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
    m_energyDeltaThreshold (0.15),
    m_stateSuppressWindowSec (15.0),
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
    m_heartbeatSendSeq (0),
    m_lastSyncedCommittedPrimaryId (0),
    m_backupHeartbeatGraceUntilTs (-1.0),
    m_failoverPending (false),
    m_failoverStartTs (0.0),
    m_joinTime (0.0),
    m_scoreFilterInit (false),
    m_initialEnergyRef (0.9),
    m_lteIsEnb (false),
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
    m_aggregateSentBytes (0),
    m_initialScoreFloodSent (false),
    m_postJoinElectEvaluated (true),
    m_lastSpnBatteryNorm (0.0),
    m_lastSpnConnNorm (0.0),
    m_lastSpnStabNorm (0.0),
    m_staticComputeCapability (1.0),
    m_queueOccupancyRatio (0.0),
    m_spnDeltaThreshold (DELTA_TH),
    m_isClusterHead (false),
    m_chScoreSaved (),
    m_chPeriodicEvent ()
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
const double HeterogeneousNodeApp::HEARTBEAT_SEC = 0.1;          // 心跳周期 100ms（论文/实验参数）
const uint8_t HeterogeneousNodeApp::HEARTBEAT_MISS_THRESHOLD = 3; // 连续 3 次丢失判定主SPN失效
const double HeterogeneousNodeApp::SPN_SWITCH_HYSTERESIS = 0.2; // 旧代理切换阈值
const double HeterogeneousNodeApp::V_TH = 10.0;                 // 速度惩罚阈值 (m/s)
const double HeterogeneousNodeApp::LAMBDA = 0.5;                // Sigmoid 陡峭系数
const double HeterogeneousNodeApp::ALPHA = 0.5;                 // L_topo 中 mobility 权重
const double HeterogeneousNodeApp::BETA = 0.5;                  // L_topo 中 struct 权重
const double HeterogeneousNodeApp::GAMMA = 0.6;                 // Adhoc 结构一跳权重
const double HeterogeneousNodeApp::SINR_MIN = 0.0;              // dB
const double HeterogeneousNodeApp::SINR_MAX = 30.0;             // dB
const double HeterogeneousNodeApp::DELTA_TH = 0.05;             // SPN 切换迟滞阈值
const double HeterogeneousNodeApp::SCORE_INPUT_EMA_ALPHA = 0.22;   // 输入平滑：抑制 linkQ 小幅抖动
const double HeterogeneousNodeApp::SCORE_OUTPUT_EMA_ALPHA = 0.35;  // 输出平滑：Hello/泛洪用分更稳
const double HeterogeneousNodeApp::SPN_PRIMARY_SCORE_THRESHOLD = 0.45; // 主SPN得分低于此阈值才允许一次得分降级切换
const double HeterogeneousNodeApp::SPN_INITIAL_ELECTION_WARMUP_SEC = 3.0; // 初始 3s 等待邻居分数同步后再锁定主备
const double HeterogeneousNodeApp::SPN_INITIAL_ELECTION_DEADLINE_SEC = 15.0; // 首轮选举最长等待（秒）
const uint32_t HeterogeneousNodeApp::SPN_PRIMARY_STABLE_TICKS = 5; // 约 5 个发送周期后才切换 committed 主
std::map<uint8_t, HeterogeneousNodeApp::SharedSpnState> HeterogeneousNodeApp::s_sharedSpnState;
HeterogeneousNodeApp::SharedAdhocChState HeterogeneousNodeApp::s_adhocChState;
std::string HeterogeneousNodeApp::s_pendingChElectReason;
EventId HeterogeneousNodeApp::s_deferredChElectEvent;
uint32_t HeterogeneousNodeApp::s_expectedAdhocInitialMembers = 0;
uint32_t HeterogeneousNodeApp::s_expectedDatalinkInitialMembers = 0;
bool HeterogeneousNodeApp::s_globalFailoverUsed = false;
bool HeterogeneousNodeApp::s_globalEnergyUsed = false;
const std::map<HeterogeneousNodeApp::SubnetType, HeterogeneousNodeApp::WeightVector>
HeterogeneousNodeApp::SUBNET_WEIGHTS = {
  { HeterogeneousNodeApp::SUBNET_ADHOC,    {0.35, 0.30, 0.20, 0.15} },
  { HeterogeneousNodeApp::SUBNET_DATALINK, {0.25, 0.25, 0.30, 0.20} },
  { HeterogeneousNodeApp::SUBNET_LTE,      {0.30, 0.20, 0.30, 0.20} }
};

void
HeterogeneousNodeApp::ResetSharedElectionState ()
{
  s_sharedSpnState.clear ();
  s_adhocChState = SharedAdhocChState ();
  s_pendingChElectReason.clear ();
  if (s_deferredChElectEvent.IsRunning ())
    {
      Simulator::Cancel (s_deferredChElectEvent);
    }
  s_deferredChElectEvent = EventId ();
  s_globalFailoverUsed = false;
  s_globalEnergyUsed = false;
  s_expectedAdhocInitialMembers = 0;
  s_expectedDatalinkInitialMembers = 0;
}

void
HeterogeneousNodeApp::SetExpectedInitialElectionMembers (SubnetType st, uint32_t count)
{
  if (st == SUBNET_ADHOC)
    {
      s_expectedAdhocInitialMembers = count;
    }
  else if (st == SUBNET_DATALINK)
    {
      s_expectedDatalinkInitialMembers = count;
    }
}

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
  m_postJoinElectEvaluated = (m_joinTime <= 0.0);
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
HeterogeneousNodeApp::SetSpnElectionTunables (double electionTimeoutSec, uint8_t heartbeatMissThreshold, double deltaTh)
{
  if (electionTimeoutSec > 0.0)
    {
      m_spnScoreStaleSec = electionTimeoutSec;
    }
  if (heartbeatMissThreshold >= 1)
    {
      m_heartbeatMissThreshold = heartbeatMissThreshold;
    }
  if (deltaTh > 0.0)
    {
      m_spnDeltaThreshold = std::min (1.0, std::max (0.0, deltaTh));
    }
}

void
HeterogeneousNodeApp::SetStaticComputeCapability (double capability)
{
  m_staticComputeCapability = std::min (1.0, std::max (0.01, capability));
}

void
HeterogeneousNodeApp::SetInitialUvMib (double energy, double linkQuality)
{
  if (energy >= 0.0)
    {
      m_uvMib.m_energy = energy;
      m_uvMib.m_lastReportedEnergy = energy;
      m_initialEnergyRef = std::max (energy, 1e-9);
    }
  if (linkQuality >= 0.0 && linkQuality <= 1.0)
    m_uvMib.m_linkQuality = linkQuality;
}

void
HeterogeneousNodeApp::ApplyScenarioInjectEnergy (double energy)
{
  double e = std::min (1.0, std::max (0.0, energy));
  m_uvMib.m_energy = e;
  m_uvMib.m_lastReportedEnergy = e;
}

void
HeterogeneousNodeApp::ApplyScenarioInjectLinkQuality (double linkQuality)
{
  m_uvMib.m_linkQuality = std::min (1.0, std::max (0.0, linkQuality));
}

void
HeterogeneousNodeApp::SetLteNodeKind (bool isEnb)
{
  m_lteIsEnb = isEnb;
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
  const double aOut = SCORE_OUTPUT_EMA_ALPHA;
  const bool firstCall = !m_scoreFilterInit;
  Ptr<MobilityModel> mob = GetNode ()->GetObject<MobilityModel> ();
  double speedMps = 0.0;
  if (mob)
    {
      Vector v = mob->GetVelocity ();
      speedMps = std::sqrt (v.x * v.x + v.y * v.y + v.z * v.z);
    }

  SpnCandidateInfo info;
  info.nodeId = GetNode ()->GetId ();
  info.subnetType = m_subnetType;
  info.L_energy = std::min (1.0, std::max (0.0, m_uvMib.m_energy / std::max (m_initialEnergyRef, 1e-9)));
  std::vector<uint32_t> oneHop = GetOneHopNeighbors ();
  std::vector<uint32_t> twoHop = GetTwoHopNeighborsProxy ();
  uint32_t clusterSize = GetSubnetClusterSize ();
  info.L_mobility = ComputeLmobility (speedMps); // Sigmoid 速度稳定度
  info.L_struct = ComputeLstruct (info.nodeId, m_subnetType, oneHop, twoHop, clusterSize); // 子网差异结构连通
  info.L_topo = ComputeLtopo (info.nodeId, m_subnetType, speedMps, oneHop, twoHop, clusterSize); // 拓扑复合因子
  info.L_link = ComputeLlink (info.nodeId, BuildNeighborSinrProxyMap ()); // SINR 线性归一化
  info.L_comp_static = m_staticComputeCapability;
  info.rho_queue = m_queueOccupancyRatio;
  info.L_comp = ComputeLcomp (info.L_comp_static, info.rho_queue, &info.disqualified); // 队列满直接失格
  info.totalScore = ComputeSpnScore (info, m_subnetType);

  double rawScore = std::min (1.0, std::max (0.0, info.totalScore));
  if (firstCall)
    {
      m_emaFinalScore = rawScore;
      m_scoreFilterInit = true;
    }
  else
    {
      m_emaFinalScore = aOut * rawScore + (1.0 - aOut) * m_emaFinalScore;
    }

  m_lastSpnBatteryNorm = info.L_energy;
  m_lastSpnConnNorm = info.L_topo;
  m_lastSpnStabNorm = info.L_link;
  double score = std::min (1.0, std::max (0.0, m_emaFinalScore));
  NS_LOG_INFO ("[Election] Node " << info.nodeId
                                  << " score=" << std::fixed << std::setprecision (3) << score
                                  << " raw=" << rawScore
                                  << " E=" << info.L_energy
                                  << " Topo=" << info.L_topo
                                  << " Link=" << info.L_link
                                  << " Comp=" << info.L_comp
                                  << " qOcc=" << info.rho_queue
                                  << " disq=" << (info.disqualified ? 1 : 0));
  return score;
}

double
HeterogeneousNodeApp::ComputeLmobility (double speedMps) const
{
  // L_mobility = 1 / (1 + exp(lambda * (v - v_th)))
  return std::min (1.0, std::max (0.0, 1.0 / (1.0 + std::exp (LAMBDA * (speedMps - V_TH)))));
}

double
HeterogeneousNodeApp::ComputeLstruct (uint32_t, SubnetType subnetType,
                                      const std::vector<uint32_t>& oneHopNeighbors,
                                      const std::vector<uint32_t>& twoHopNeighbors,
                                      uint32_t clusterSize) const
{
  if (clusterSize <= 1)
    {
      return 0.0;
    }
  const double n1 = static_cast<double> (oneHopNeighbors.size ());
  if (subnetType == SUBNET_DATALINK)
    {
      return std::min (1.0, std::max (0.0, n1 / static_cast<double> (clusterSize - 1)));
    }
  if (subnetType == SUBNET_ADHOC)
    {
      const double n2 = static_cast<double> (twoHopNeighbors.size ());
      const double oneHopNorm = n1 / static_cast<double> (clusterSize - 1);
      const double twoHopNorm = n2 / static_cast<double> (clusterSize);
      return std::min (1.0, std::max (0.0, GAMMA * oneHopNorm + (1.0 - GAMMA) * twoHopNorm));
    }
  return 0.0; // LTE 不使用结构项
}

double
HeterogeneousNodeApp::ComputeLtopo (uint32_t nodeId, SubnetType subnetType, double speedMps,
                                    const std::vector<uint32_t>& oneHopNeighbors,
                                    const std::vector<uint32_t>& twoHopNeighbors,
                                    uint32_t clusterSize) const
{
  const double lMob = ComputeLmobility (speedMps);
  if (subnetType == SUBNET_LTE)
    {
      return lMob; // LTE: L_topo = L_mobility
    }
  const double lStruct = ComputeLstruct (nodeId, subnetType, oneHopNeighbors, twoHopNeighbors, clusterSize);
  return std::min (1.0, std::max (0.0, ALPHA * lMob + BETA * lStruct));
}

double
HeterogeneousNodeApp::ComputeLlink (uint32_t, const std::map<uint32_t, double>& neighborSinrMap) const
{
  if (neighborSinrMap.empty ())
    {
      return std::min (1.0, std::max (0.0, m_uvMib.m_linkQuality));
    }
  double sum = 0.0;
  for (const auto& kv : neighborSinrMap)
    {
      double norm = (kv.second - SINR_MIN) / std::max (1e-9, (SINR_MAX - SINR_MIN));
      sum += std::min (1.0, std::max (0.0, norm));
    }
  return std::min (1.0, std::max (0.0, sum / static_cast<double> (neighborSinrMap.size ())));
}

double
HeterogeneousNodeApp::ComputeLcomp (double staticCapability, double queueOccupancy, bool* disqualified) const
{
  const double q = std::min (1.0, std::max (0.0, queueOccupancy));
  if (disqualified)
    {
      *disqualified = (q >= 1.0);
    }
  if (q >= 1.0)
    {
      return 0.0;
    }
  const double cappedStatic = std::min (1.0, std::max (0.01, staticCapability));
  return std::min (1.0, std::max (0.0, cappedStatic * (1.0 - q)));
}

double
HeterogeneousNodeApp::ComputeSpnScore (const SpnCandidateInfo& info, SubnetType subnetType) const
{
  auto it = SUBNET_WEIGHTS.find (subnetType);
  WeightVector w = (it == SUBNET_WEIGHTS.end ()) ? WeightVector{0.35, 0.30, 0.20, 0.15} : it->second;
  return w.wEnergy * info.L_energy + w.wTopo * info.L_topo + w.wLink * info.L_link + w.wComp * info.L_comp;
}

std::vector<uint32_t>
HeterogeneousNodeApp::GetOneHopNeighbors () const
{
  std::vector<uint32_t> oneHop;
  oneHop.reserve (m_neighborAddrs.size ());
  for (const auto& kv : m_neighborAddrs)
    {
      oneHop.push_back (kv.first);
    }
  return oneHop;
}

std::vector<uint32_t>
HeterogeneousNodeApp::GetTwoHopNeighborsProxy () const
{
  std::set<uint32_t> oneHopSet;
  std::set<uint32_t> twoHopSet;
  const uint32_t selfId = GetNode ()->GetId ();
  for (const auto& kv : m_neighborAddrs)
    {
      oneHopSet.insert (kv.first);
    }
  for (uint32_t nb : oneHopSet)
    {
      Ptr<HeterogeneousNodeApp> app = FindHetAppOnNode (nb, m_subnetType);
      if (!app)
        {
          continue;
        }
      for (const auto& n2 : app->m_neighborAddrs)
        {
          if (n2.first == selfId || oneHopSet.count (n2.first) != 0u)
            {
              continue;
            }
          twoHopSet.insert (n2.first);
        }
    }
  return std::vector<uint32_t> (twoHopSet.begin (), twoHopSet.end ());
}

uint32_t
HeterogeneousNodeApp::GetSubnetClusterSize () const
{
  uint32_t total = 0;
  for (uint32_t i = 0; i < NodeList::GetNNodes (); ++i)
    {
      Ptr<HeterogeneousNodeApp> app = FindHetAppOnNode (i, m_subnetType);
      if (app && app->m_running)
        {
          total++;
        }
    }
  return std::max (1u, total);
}

std::map<uint32_t, double>
HeterogeneousNodeApp::BuildNeighborSinrProxyMap () const
{
  std::map<uint32_t, double> out;
  // 若缺少物理层 SINR 监控，则使用已有 linkQuality 作为代理线性映射到 [0,30] dB。
  for (const auto& kv : m_neighborAddrs)
    {
      double sinrProxy = std::min (SINR_MAX, std::max (SINR_MIN, m_uvMib.m_linkQuality * SINR_MAX));
      out[kv.first] = sinrProxy;
    }
  return out;
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
              m_backupHeartbeatGraceUntilTs = -1.0;
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
      if (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK)
        {
          SharedSpnState& sh = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
          sh.scoreByNode[neighborNodeId] = neighborScore;
          sh.scoreTimeByNode[neighborNodeId] = now;
        }

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
  // 分数静默合并；选举仅在 MaybeTriggerSpnElection 条件满足时触发（非每次泛洪）
  SpnSyncScoresOnly (myScore);
  MaybeTriggerSpnElection (myScore);
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
      uint32_t backupId = m_committedBackupId;
      if (backupId != 0 && backupId != selfId)
        {
          auto itB = m_neighborAddrs.find (backupId);
          if (itB != m_neighborAddrs.end ())
            {
              Ptr<Packet> pktU = Create<Packet> (buf, len);
              m_helloSocket->SendTo (pktU, 0, InetSocketAddress (itB->second, m_helloPort));
            }
        }
      m_heartbeatSendSeq++;
      uint32_t tgt = m_committedBackupId;
      if (tgt == 0 || tgt == selfId)
        {
          tgt = m_committedPrimaryId;
        }
      NMS_LOG_INFO (selfId, "HEARTBEAT_SEND",
                    "target=" + std::to_string (tgt) + " seq=" + std::to_string (m_heartbeatSendSeq));
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
      if (m_backupHeartbeatGraceUntilTs >= 0.0 && now < m_backupHeartbeatGraceUntilTs)
        {
          m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC), &HeterogeneousNodeApp::CheckPrimaryHeartbeat, this);
          return;
        }
      // 略大于 HEARTBEAT_SEC 的判定裕量，避免调度抖动导致误计丢心跳
      const double hbGrace = HEARTBEAT_SEC * 2.5;
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
              RunFullElection ("heartbeat_primary_lost_failover");
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
HeterogeneousNodeApp::BroadcastScoreFloodCurrent ()
{
  if (!m_running || !m_helloSocket)
    {
      return;
    }
  SendScoreFlood (CalculateUtilityScore ());
}

void
HeterogeneousNodeApp::SpnMergeScoresIntoShared (double myScore)
{
  if (m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK)
    {
      return;
    }
  uint32_t selfId = GetNode ()->GetId ();
  double now = Simulator::Now ().GetSeconds ();
  m_globalScores[selfId] = myScore;
  m_globalScoreTime[selfId] = now;
  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
  shared.scoreByNode[selfId] = myScore;
  shared.scoreTimeByNode[selfId] = now;
  for (const auto& kv : m_globalScores)
    {
      auto itT = m_globalScoreTime.find (kv.first);
      double ts = (itT != m_globalScoreTime.end ()) ? itT->second : 0.0;
      double age = now - ts;
      if (age <= m_spnScoreStaleSec && age >= 0.0)
        {
          shared.scoreByNode[kv.first] = kv.second;
          shared.scoreTimeByNode[kv.first] = ts;
        }
    }
}

void
HeterogeneousNodeApp::ResyncSpnHeartbeatTimerFromRoleTransition (bool oldIsSpn, bool oldBackup)
{
  if ((m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK) || !m_running)
    {
      return;
    }
  const bool nowPrimary = m_isSpn;
  const bool nowBackup = m_isBackupSpn;
  if (oldIsSpn == nowPrimary && oldBackup == nowBackup)
    {
      return;
    }
  uint32_t selfId = GetNode ()->GetId ();
  if (m_heartbeatEvent.IsRunning ())
    {
      Simulator::Cancel (m_heartbeatEvent);
    }
  if (nowPrimary)
    {
      if (oldBackup && nowPrimary)
        {
          NMS_LOG_INFO (selfId, "TAKEOVER_COMPLETE",
                        "new_primary=" + std::to_string (selfId) + " subnet=" +
                        std::to_string (static_cast<uint32_t> (m_subnetType)));
        }
      if (!m_flushEvent.IsRunning ())
        {
          m_flushEvent = Simulator::Schedule (m_aggregateInterval,
                                              &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
        }
      if (m_failoverPending)
        {
          double healSec = Simulator::Now ().GetSeconds () - m_failoverStartTs;
          NMS_LOG_INFO (selfId, "SELF_HEAL_TIME", "takeover completed in " + std::to_string (healSec) + "s");
          m_failoverPending = false;
        }
      Simulator::ScheduleNow (&HeterogeneousNodeApp::SendHeartbeatSync, this);
    }
  else if (nowBackup)
    {
      NMS_LOG_INFO (selfId, "SPN_ELECT", "elected as backup SPN");
      m_missedHeartbeats = 0;
      m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC),
                                              &HeterogeneousNodeApp::CheckPrimaryHeartbeat, this);
    }
  else if (oldIsSpn && !nowPrimary)
    {
      NMS_LOG_INFO (selfId, "SPN_SWITCH", "lost primary SPN role");
      m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC),
                                              &HeterogeneousNodeApp::CheckPrimaryHeartbeat, this);
    }
  else
    {
      m_heartbeatEvent = Simulator::Schedule (Seconds (HEARTBEAT_SEC),
                                              &HeterogeneousNodeApp::CheckPrimaryHeartbeat, this);
    }
}

void
HeterogeneousNodeApp::ApplyLocalRoleFromShared ()
{
  if (m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK)
    {
      return;
    }
  const bool oldIsSpn = m_isSpn;
  const bool oldBackup = m_isBackupSpn;
  uint32_t selfId = GetNode ()->GetId ();
  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
  m_initialSpnLocked = shared.initialized;
  m_failoverSwitchUsed = shared.failoverUsed;
  m_energySwitchUsed = shared.energySwitchUsed;
  m_committedPrimaryId = shared.primaryId;
  m_committedBackupId = shared.backupId;
  if (m_committedPrimaryId == 0)
    {
      return;
    }
  uint32_t spnNodeId = m_committedPrimaryId;
  uint32_t backupNodeId = (m_committedBackupId == 0) ? spnNodeId : m_committedBackupId;
  m_currentPrimaryId = spnNodeId;
  m_isSpn = (spnNodeId == selfId);
  m_isBackupSpn = (backupNodeId == selfId && !m_isSpn);
  if (m_committedPrimaryId != m_lastSyncedCommittedPrimaryId)
    {
      if (m_isBackupSpn && m_lastSyncedCommittedPrimaryId != 0)
        {
          m_missedHeartbeats = 0;
          m_lastPrimaryHeartbeatTs = Simulator::Now ().GetSeconds ();
          m_backupHeartbeatGraceUntilTs =
              Simulator::Now ().GetSeconds () + static_cast<double> (m_heartbeatMissThreshold) * HEARTBEAT_SEC + HEARTBEAT_SEC;
        }
      m_lastSyncedCommittedPrimaryId = m_committedPrimaryId;
    }
  if (oldIsSpn != m_isSpn || oldBackup != m_isBackupSpn)
    {
      ResyncSpnHeartbeatTimerFromRoleTransition (oldIsSpn, oldBackup);
    }
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
HeterogeneousNodeApp::SpnSyncScoresOnly (double myScore)
{
  SpnMergeScoresIntoShared (myScore);
  ApplyLocalRoleFromShared ();
}

void
HeterogeneousNodeApp::MaybeTriggerSpnElection (double myScore)
{
  if (m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK)
    {
      return;
    }
  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
  double now = Simulator::Now ().GetSeconds ();

  // 首轮选举由 StartApplication 中 RunInitialLockElection（join_time+0.1s）统一触发，reason=initial_lock

  // 条件 A：迟入网节点（无 MARGIN，按 score 与 Primary/Backup 比较分三种情况）
  if (m_joinTime > 0.0 && !m_postJoinElectEvaluated && now >= m_joinTime)
    {
      m_postJoinElectEvaluated = true;
      uint32_t pid = shared.primaryId;
      double ps = 0.0;
      auto itp = shared.scoreByNode.find (pid);
      if (itp != shared.scoreByNode.end ())
        {
          ps = itp->second;
        }
      double bs = -1.0;
      if (shared.backupId != 0 && shared.backupId != pid)
        {
          auto itb = shared.scoreByNode.find (shared.backupId);
          if (itb != shared.scoreByNode.end ())
            {
              bs = itb->second;
            }
        }
      if (myScore > ps)
        {
          RunFullElection ("late_join_rank_insert_primary");
        }
      else if (bs >= 0.0 && myScore > bs && myScore <= ps)
        {
          RunFullElection ("late_join_rank_insert_backup");
        }
      // else：new_score ≤ Backup_score → 仅作 TSN，不触发选举
      // 迟入网后仍须重算簇首（新节点 CH_Score 可能高于当前 CH）
      if (m_subnetType == SUBNET_ADHOC)
        {
          s_pendingChElectReason = "late_join_ch";
          if (s_deferredChElectEvent.IsRunning ())
            {
              Simulator::Cancel (s_deferredChElectEvent);
            }
          s_deferredChElectEvent =
              Simulator::Schedule (MilliSeconds (250), &HeterogeneousNodeApp::RunClusterHeadElectionGlobal);
        }
      return;
    }

  // 条件 C 在 Primary 的 OnConditionCEval 中处理（与泛洪路径解耦）
}

void
HeterogeneousNodeApp::RunFullElection (const std::string& electReason)
{
  if (m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK)
    return;

  uint32_t selfId = GetNode ()->GetId ();
  double myScore = CalculateUtilityScore ();
  double now = Simulator::Now ().GetSeconds ();

  SpnMergeScoresIntoShared (myScore);

  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
  const uint32_t electPrevPrimary = shared.primaryId;

  std::vector<std::pair<double, uint32_t>> candidates;
  bool currentPrimaryDisqualified = false;
  double currentPrimaryScore = -1.0;
  for (const auto& kv : shared.scoreByNode)
    {
      double age = now - shared.scoreTimeByNode[kv.first];
      if (age > m_spnScoreStaleSec)
        {
          continue;
        }
      Ptr<HeterogeneousNodeApp> candApp = FindHetAppOnNode (kv.first, m_subnetType);
      bool disqByQueue = (candApp && candApp->m_queueOccupancyRatio >= 1.0);
      if (shared.initialized && kv.first == shared.primaryId)
        {
          if (!candApp || !candApp->m_running || disqByQueue)
            {
              currentPrimaryDisqualified = true;
            }
          else
            {
              currentPrimaryScore = kv.second;
            }
        }
      if (!disqByQueue)
        {
          candidates.push_back (std::make_pair (kv.second, kv.first));
        }
    }
  if (candidates.empty ())
    candidates.push_back (std::make_pair (myScore, selfId));

  // Score 降序；同分 nodeId 升序，保证确定性
  std::sort (candidates.begin (), candidates.end (),
             [] (const std::pair<double, uint32_t>& a, const std::pair<double, uint32_t>& b) {
               if (a.first != b.first) return a.first > b.first;
               return a.second < b.second;
             });

  uint32_t newPrimary = candidates[0].second;
  uint32_t newBackup = (candidates.size () >= 2) ? candidates[1].second : newPrimary;
  double primaryScoreVal = candidates[0].first;
  double backupScoreVal = (candidates.size () >= 2) ? candidates[1].first : primaryScoreVal;

  bool stateChanged = false;
  std::string changeReason;

  uint32_t need = (m_subnetType == SUBNET_ADHOC) ? s_expectedAdhocInitialMembers : s_expectedDatalinkInitialMembers;
  if (need == 0)
    need = 2;

  if (!shared.initialized)
    {
      if (candidates.size () < need && now < SPN_INITIAL_ELECTION_DEADLINE_SEC)
        {
          return;
        }
      shared.primaryId = newPrimary;
      shared.backupId = newBackup;
      shared.initialized = true;
      shared.initializedTs = now;
      shared.stablePhase = true;
      stateChanged = true;
      changeReason = electReason.empty () ? "init_phase_election" : electReason;
    }
  else
    {
      if (newPrimary != shared.primaryId)
        {
          if (!currentPrimaryDisqualified)
            {
              // 迟滞：仅当候选分数显著高于当前主 SPN 才切换
              if (currentPrimaryScore < 0.0)
                {
                  auto itCurr = shared.scoreByNode.find (shared.primaryId);
                  if (itCurr != shared.scoreByNode.end ())
                    {
                      currentPrimaryScore = itCurr->second;
                    }
                }
              if (currentPrimaryScore >= 0.0 && (primaryScoreVal - currentPrimaryScore) <= m_spnDeltaThreshold)
                {
                  newPrimary = shared.primaryId;
                  primaryScoreVal = currentPrimaryScore;
                  for (const auto& c : candidates)
                    {
                      if (c.second != newPrimary)
                        {
                          newBackup = c.second;
                          backupScoreVal = c.first;
                          break;
                        }
                    }
                }
            }
        }

      if (newPrimary != shared.primaryId || newBackup != shared.backupId || currentPrimaryDisqualified)
        {
          shared.primaryId = newPrimary;
          shared.backupId = newBackup;
          stateChanged = true;
          if (currentPrimaryDisqualified)
            {
              changeReason = electReason.empty () ? "forced_switch_primary_disqualified" : electReason;
            }
          else
            {
              changeReason = electReason.empty () ? "stable_full_reselect" : electReason;
            }
        }
    }

  m_initialSpnLocked = shared.initialized;
  m_failoverSwitchUsed = shared.failoverUsed;
  m_energySwitchUsed = shared.energySwitchUsed;
  m_committedPrimaryId = shared.primaryId;
  m_committedBackupId = shared.backupId;

  if (m_committedPrimaryId == 0)
    return;

  if (stateChanged)
    {
      SpnElectHistoryEntry rec;
      rec.timeSec = now;
      rec.primaryId = newPrimary;
      rec.backupId = newBackup;
      rec.reason = changeReason;
      shared.electHistory.push_back (rec);
      if (shared.electHistory.size () > 200u)
        {
          shared.electHistory.erase (shared.electHistory.begin ());
        }
    }

  // 记录一次完整选举计算；条件 C 挑战计数在完整重选后清零
  shared.lastElectTs = now;
  shared.challengeChallengerId = 0;
  shared.challengeConsecutive = 0;

  uint32_t spnNodeId = m_committedPrimaryId;
  uint32_t backupNodeId = (m_committedBackupId == 0) ? spnNodeId : m_committedBackupId;

  m_currentPrimaryId = spnNodeId;

  bool wasSpn = m_isSpn;
  bool wasBackup = m_isBackupSpn;
  m_isSpn = (spnNodeId == selfId);
  m_isBackupSpn = (backupNodeId == selfId && !m_isSpn);
  if (m_isBackupSpn && electPrevPrimary != 0 && spnNodeId != electPrevPrimary)
    {
      m_missedHeartbeats = 0;
      m_lastPrimaryHeartbeatTs = Simulator::Now ().GetSeconds ();
      m_backupHeartbeatGraceUntilTs =
          Simulator::Now ().GetSeconds () + static_cast<double> (m_heartbeatMissThreshold) * HEARTBEAT_SEC + HEARTBEAT_SEC;
    }
  m_lastSyncedCommittedPrimaryId = m_committedPrimaryId;

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
        {
          NMS_LOG_INFO (selfId, "SPN_TAKEOVER", std::string ("Node ") + std::to_string (selfId) + " takes over as primary SPN");
          NMS_LOG_INFO (selfId, "TAKEOVER_COMPLETE",
                        "new_primary=" + std::to_string (selfId) + " subnet=" +
                        std::to_string (static_cast<uint32_t> (m_subnetType)));
        }
      if (!m_flushEvent.IsRunning ())
        m_flushEvent = Simulator::Schedule (m_aggregateInterval,
                                            &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
      if (m_heartbeatEvent.IsRunning ())
        Simulator::Cancel (m_heartbeatEvent);
      Simulator::ScheduleNow (&HeterogeneousNodeApp::SendHeartbeatSync, this);
      // SPN_ANNOUNCE / Score 泛洪见下方 stateChanged && selfId==newPrimary，避免重复
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

  // 由当选 Primary 发 SPN_ANNOUNCE + Score 泛洪；若本轮 RunFullElection 的调用者不是 Primary，则调度到 Primary 上输出（修复 DataLink 等子网无宣告）
  if (stateChanged)
    {
      if (selfId == newPrimary)
        {
          std::ostringstream oss;
          oss << "subnet=" << static_cast<uint32_t> (m_subnetType)
              << " primary=" << spnNodeId
              << " backup=" << backupNodeId
              << " reason=" << changeReason
              << " primaryScore=" << std::fixed << std::setprecision (3) << primaryScoreVal
              << " backupScore=" << std::setprecision (3) << backupScoreVal;
          NMS_LOG_INFO (selfId, "SPN_ANNOUNCE", oss.str ());
          const char* subLbl = (m_subnetType == SUBNET_DATALINK) ? "DataLink" : "Adhoc";
          std::ostringstream spnElect;
          spnElect << std::fixed << std::setprecision (1) << "t=" << now << "s [SPN_ELECT] SubNet: " << subLbl
                   << "\n  Primary: Node " << spnNodeId << " (score=" << std::setprecision (2) << primaryScoreVal << ")"
                   << "\n  Backup:  Node " << backupNodeId << " (score=" << std::setprecision (2) << backupScoreVal << ")"
                   << "\n  触发原因: " << changeReason
                   << "\n  elected as primary SPN (score=" << std::setprecision (3) << myScore << ")";
          NMS_LOG_INFO (selfId, "SPN_ELECT", spnElect.str ());
          SendScoreFlood (myScore);
          if (m_isSpn)
            {
              Simulator::Schedule (MilliSeconds (50), &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
            }
        }
      else
        {
          Ptr<HeterogeneousNodeApp> priApp = FindHetAppOnNode (newPrimary, m_subnetType);
          if (priApp)
            {
              Simulator::Schedule (Seconds (0), &HeterogeneousNodeApp::PublishSpnAnnounceAfterElection, priApp,
                                   primaryScoreVal, backupScoreVal, changeReason);
            }
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

  SyncConditionCTimer ();
  if (m_subnetType == SUBNET_ADHOC && stateChanged)
    {
      s_pendingChElectReason = "after_spn_elect";
      Simulator::Schedule (MilliSeconds (90), &HeterogeneousNodeApp::RunClusterHeadElectionGlobal);
    }
}

Ptr<HeterogeneousNodeApp>
HeterogeneousNodeApp::FindHetAppOnNode (uint32_t nodeId, SubnetType st)
{
  Ptr<Node> n = NodeList::GetNode (nodeId);
  if (!n)
    {
      return nullptr;
    }
  for (uint32_t i = 0; i < n->GetNApplications (); ++i)
    {
      Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> (n->GetApplication (i));
      if (app && app->GetSubnetType () == st)
        {
          return app;
        }
    }
  return nullptr;
}

void
HeterogeneousNodeApp::PublishSpnAnnounceAfterElection (double primaryScoreVal, double backupScoreVal,
                                                         const std::string& changeReason)
{
  if (!m_running || (m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK))
    {
      return;
    }
  ApplyLocalRoleFromShared ();
  uint32_t selfId = GetNode ()->GetId ();
  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
  uint32_t spnNodeId = shared.primaryId;
  if (selfId != spnNodeId)
    {
      return;
    }
  uint32_t backupNodeId = (shared.backupId == 0) ? spnNodeId : shared.backupId;
  double myScore = CalculateUtilityScore ();
  double now = Simulator::Now ().GetSeconds ();
  std::ostringstream oss;
  oss << "subnet=" << static_cast<uint32_t> (m_subnetType)
      << " primary=" << spnNodeId
      << " backup=" << backupNodeId
      << " reason=" << changeReason
      << " primaryScore=" << std::fixed << std::setprecision (3) << primaryScoreVal
      << " backupScore=" << std::setprecision (3) << backupScoreVal;
  NMS_LOG_INFO (selfId, "SPN_ANNOUNCE", oss.str ());
  const char* subLbl = (m_subnetType == SUBNET_DATALINK) ? "DataLink" : "Adhoc";
  std::ostringstream spnElect;
  spnElect << std::fixed << std::setprecision (1) << "t=" << now << "s [SPN_ELECT] SubNet: " << subLbl
           << "\n  Primary: Node " << spnNodeId << " (score=" << std::setprecision (2) << primaryScoreVal << ")"
           << "\n  Backup:  Node " << backupNodeId << " (score=" << std::setprecision (2) << backupScoreVal << ")"
           << "\n  触发原因: " << changeReason
           << "\n  elected as primary SPN (score=" << std::setprecision (3) << myScore << ")";
  NMS_LOG_INFO (selfId, "SPN_ELECT", spnElect.str ());
  SendScoreFlood (myScore);
  if (m_isSpn)
    {
      Simulator::Schedule (MilliSeconds (50), &HeterogeneousNodeApp::FlushAggregatedToGmc, this);
    }
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
  SyncConditionCTimer ();
  if (m_subnetType == SUBNET_ADHOC)
    {
      SyncAdhocChPeriodicTimer ();
    }
}

void
HeterogeneousNodeApp::RunInitialLockElection (void)
{
  if (!m_running || (m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK))
    {
      return;
    }
  RunFullElection ("initial_lock");
}

void
HeterogeneousNodeApp::SpnForgetPeer (uint32_t peerNodeId)
{
  m_globalScores.erase (peerNodeId);
  m_globalScoreTime.erase (peerNodeId);
  if (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK)
    {
      SharedSpnState& sh = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
      sh.scoreByNode.erase (peerNodeId);
      sh.scoreTimeByNode.erase (peerNodeId);
    }
  if (m_subnetType == SUBNET_ADHOC && s_adhocChState.clusterHeadId == peerNodeId)
    {
      s_pendingChElectReason = "ch_offline";
      if (s_deferredChElectEvent.IsRunning ())
        {
          Simulator::Cancel (s_deferredChElectEvent);
        }
      s_deferredChElectEvent =
          Simulator::Schedule (MilliSeconds (120), &HeterogeneousNodeApp::RunClusterHeadElectionGlobal);
    }
}

void
HeterogeneousNodeApp::ComputeAdhocChScoresMap (std::map<uint32_t, ChScoreBreakdownInternal>& byId,
                                               uint32_t& bestId, double& bestTotal)
{
  byId.clear ();
  bestId = 0;
  bestTotal = -1.0;
  struct Cand
  {
    Ptr<HeterogeneousNodeApp> app;
    uint32_t id;
    Vector pos;
    double energy;
    uint32_t degree;
    double mobStab;
  };
  std::vector<Cand> cands;
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      Ptr<Node> node = *it;
      Ptr<HeterogeneousNodeApp> ap = FindHetAppOnNode (node->GetId (), SUBNET_ADHOC);
      if (!ap || !ap->m_running)
        {
          continue;
        }
      Cand c;
      c.app = ap;
      c.id = node->GetId ();
      Ptr<MobilityModel> mm = node->GetObject<MobilityModel> ();
      c.pos = mm ? mm->GetPosition () : Vector (0, 0, 0);
      c.energy = ap->m_uvMib.m_energy;
      c.degree = ap->GetOneHopNeighborCount ();
      double ms = ap->m_uvMib.m_mobilityScore;
      ms = std::max (0.0, std::min (1.0, ms));
      c.mobStab = 1.0 - ms;
      cands.push_back (c);
    }
  const size_t n = cands.size ();
  if (n == 0)
    {
      return;
    }

  double cx = 0.0, cy = 0.0;
  for (const auto& c : cands)
    {
      cx += c.pos.x;
      cy += c.pos.y;
    }
  cx /= static_cast<double> (n);
  cy /= static_cast<double> (n);
  std::vector<double> rawE, rawD, rawM, rawCen;
  double maxDist = 0.0;
  std::vector<double> dists;
  for (const auto& c : cands)
    {
      double d = std::hypot (c.pos.x - cx, c.pos.y - cy);
      dists.push_back (d);
      maxDist = std::max (maxDist, d);
    }
  for (size_t i = 0; i < n; ++i)
    {
      rawE.push_back (cands[i].energy);
      rawD.push_back (static_cast<double> (cands[i].degree));
      rawM.push_back (cands[i].mobStab);
      double cen = (maxDist < 1e-9) ? 1.0 : (1.0 - dists[i] / maxDist);
      cen = std::max (0.0, std::min (1.0, cen));
      rawCen.push_back (cen);
    }
  auto normalize = [] (const std::vector<double>& raw) {
    std::vector<double> out (raw.size (), 0.5);
    if (raw.empty ())
      {
        return out;
      }
    double lo = *std::min_element (raw.begin (), raw.end ());
    double hi = *std::max_element (raw.begin (), raw.end ());
    if (hi <= lo + 1e-12)
      {
        for (size_t i = 0; i < raw.size (); ++i)
          {
            out[i] = 1.0;
          }
      }
    else
      {
        for (size_t i = 0; i < raw.size (); ++i)
          {
            out[i] = (raw[i] - lo) / (hi - lo);
          }
      }
    return out;
  };
  std::vector<double> nE = normalize (rawE);
  std::vector<double> nD = normalize (rawD);
  std::vector<double> nM = normalize (rawM);
  std::vector<double> nC = normalize (rawCen);
  for (size_t i = 0; i < n; ++i)
    {
      ChScoreBreakdownInternal br;
      br.energy = nE[i];
      br.degree = nD[i];
      br.mobility = nM[i];
      br.centrality = nC[i];
      br.total = 0.25 * br.energy + 0.35 * br.degree + 0.20 * br.mobility + 0.20 * br.centrality;
      uint32_t id = cands[i].id;
      byId[id] = br;
      if (br.total > bestTotal + 1e-15 ||
          (std::fabs (br.total - bestTotal) <= 1e-15 && (bestId == 0 || id < bestId)))
        {
          bestTotal = br.total;
          bestId = id;
        }
    }
}

void
HeterogeneousNodeApp::RunClusterHeadElectionGlobal ()
{
  std::string reason = s_pendingChElectReason.empty () ? "ch_elect" : s_pendingChElectReason;
  s_pendingChElectReason.clear ();
  s_deferredChElectEvent = EventId ();

  double now = Simulator::Now ().GetSeconds ();
  std::map<uint32_t, ChScoreBreakdownInternal> byId;
  uint32_t bestId = 0;
  double bestTotal = -1.0;
  ComputeAdhocChScoresMap (byId, bestId, bestTotal);

  if (byId.empty ())
    {
      s_adhocChState.clusterHeadId = 0;
      s_adhocChState.challengerId = 0;
      s_adhocChState.challengerConsecutive = 0;
      for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
        {
          Ptr<HeterogeneousNodeApp> ap = FindHetAppOnNode ((*it)->GetId (), SUBNET_ADHOC);
          if (ap)
            {
              ap->m_isClusterHead = false;
              ap->m_chScoreSaved = ChScoreBreakdownInternal ();
            }
        }
      SharedSpnState& sh = s_sharedSpnState[static_cast<uint8_t> (SUBNET_ADHOC)];
      Ptr<HeterogeneousNodeApp> pri = FindHetAppOnNode (sh.primaryId, SUBNET_ADHOC);
      if (pri)
        {
          pri->SyncAdhocChPeriodicTimer ();
        }
      return;
    }

  s_adhocChState.clusterHeadId = bestId;
  s_adhocChState.challengerId = 0;
  s_adhocChState.challengerConsecutive = 0;
  AdhocChElectHistoryEntry he;
  he.timeSec = now;
  he.clusterHeadId = bestId;
  he.reason = reason;
  s_adhocChState.electHistory.push_back (he);
  if (s_adhocChState.electHistory.size () > 200u)
    {
      s_adhocChState.electHistory.erase (s_adhocChState.electHistory.begin ());
    }

  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      Ptr<HeterogeneousNodeApp> ap = FindHetAppOnNode ((*it)->GetId (), SUBNET_ADHOC);
      if (!ap)
        {
          continue;
        }
      uint32_t id = ap->GetNode ()->GetId ();
      ap->m_isClusterHead = (id == bestId && ap->m_running);
      auto itb = byId.find (id);
      if (itb != byId.end ())
        {
          ap->m_chScoreSaved = itb->second;
        }
      else
        {
          ap->m_chScoreSaved = ChScoreBreakdownInternal ();
        }
    }

  auto itb = byId.find (bestId);
  std::ostringstream oss;
  if (itb != byId.end ())
    {
      const ChScoreBreakdownInternal& b = itb->second;
      oss << "subnet=1 cluster_head=" << bestId << " total=" << std::fixed << std::setprecision (4) << b.total
          << " energy=" << std::setprecision (4) << b.energy << " degree=" << b.degree
          << " mobility=" << b.mobility << " centrality=" << b.centrality << " reason=" << reason;
    }
  else
    {
      oss << "subnet=1 cluster_head=" << bestId << " reason=" << reason;
    }
  NMS_LOG_INFO (bestId, "CH_ANNOUNCE", oss.str ());

  SharedSpnState& sh = s_sharedSpnState[static_cast<uint8_t> (SUBNET_ADHOC)];
  Ptr<HeterogeneousNodeApp> pri = FindHetAppOnNode (sh.primaryId, SUBNET_ADHOC);
  if (pri)
    {
      pri->SyncAdhocChPeriodicTimer ();
    }
}

void
HeterogeneousNodeApp::SyncAdhocChPeriodicTimer ()
{
  if (m_subnetType != SUBNET_ADHOC)
    {
      return;
    }
  if (m_chPeriodicEvent.IsRunning ())
    {
      Simulator::Cancel (m_chPeriodicEvent);
    }
  if (!m_running)
    {
      return;
    }
  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (SUBNET_ADHOC)];
  uint32_t selfId = GetNode ()->GetId ();
  if (shared.initialized && selfId == shared.primaryId)
    {
      m_chPeriodicEvent = Simulator::Schedule (Seconds (CH_EVAL_PERIOD_SEC),
                                               &HeterogeneousNodeApp::OnAdhocChPeriodicEval, this);
    }
}

void
HeterogeneousNodeApp::OnAdhocChPeriodicEval ()
{
  if (!m_running || m_subnetType != SUBNET_ADHOC)
    {
      return;
    }
  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (SUBNET_ADHOC)];
  uint32_t selfId = GetNode ()->GetId ();
  if (!shared.initialized || selfId != shared.primaryId)
    {
      return;
    }

  if (m_running)
    {
      m_chPeriodicEvent = Simulator::Schedule (Seconds (CH_EVAL_PERIOD_SEC),
                                               &HeterogeneousNodeApp::OnAdhocChPeriodicEval, this);
    }

  std::map<uint32_t, ChScoreBreakdownInternal> byId;
  uint32_t bestOverall = 0;
  double bestTot = -1.0;
  ComputeAdhocChScoresMap (byId, bestOverall, bestTot);
  if (byId.empty ())
    {
      return;
    }

  uint32_t chId = s_adhocChState.clusterHeadId;
  if (chId == 0)
    {
      s_pendingChElectReason = "periodic_recover";
      RunClusterHeadElectionGlobal ();
      return;
    }

  auto itCh = byId.find (chId);
  if (itCh == byId.end ())
    {
      s_pendingChElectReason = "periodic_ch_missing";
      RunClusterHeadElectionGlobal ();
      return;
    }
  double chScore = itCh->second.total;

  uint32_t challenger = 0;
  double challengerTotal = -1.0;
  for (const auto& kv : byId)
    {
      if (kv.first == chId)
        {
          continue;
        }
      if (kv.second.total > chScore + CH_PERIODIC_SCORE_MARGIN)
        {
          if (kv.second.total > challengerTotal + 1e-15 ||
              (std::fabs (kv.second.total - challengerTotal) <= 1e-15 &&
               (challenger == 0 || kv.first < challenger)))
            {
              challengerTotal = kv.second.total;
              challenger = kv.first;
            }
        }
    }

  if (challenger == 0)
    {
      s_adhocChState.challengerId = 0;
      s_adhocChState.challengerConsecutive = 0;
      return;
    }

  if (challenger == s_adhocChState.challengerId)
    {
      s_adhocChState.challengerConsecutive++;
    }
  else
    {
      s_adhocChState.challengerId = challenger;
      s_adhocChState.challengerConsecutive = 1;
    }

  if (s_adhocChState.challengerConsecutive >= CH_PERIODIC_CONFIRM_PERIODS)
    {
      s_pendingChElectReason = "periodic_margin_two_periods";
      RunClusterHeadElectionGlobal ();
    }
}

uint32_t
HeterogeneousNodeApp::GetOneHopNeighborCount () const
{
  return static_cast<uint32_t> (m_neighborAddrs.size ());
}

bool
HeterogeneousNodeApp::IsClusterHead () const
{
  return m_isClusterHead && m_subnetType == SUBNET_ADHOC;
}

HeterogeneousNodeApp::AdhocChScoreSnapshot
HeterogeneousNodeApp::GetAdhocChScoreSnapshot () const
{
  AdhocChScoreSnapshot s;
  s.total = m_chScoreSaved.total;
  s.energy = m_chScoreSaved.energy;
  s.degree = m_chScoreSaved.degree;
  s.mobility = m_chScoreSaved.mobility;
  s.centrality = m_chScoreSaved.centrality;
  return s;
}

void
HeterogeneousNodeApp::EvaluateConditionC ()
{
  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
  double now = Simulator::Now ().GetSeconds ();
  uint32_t pid = shared.primaryId;
  if (pid == 0)
    {
      return;
    }
  auto itp = shared.scoreByNode.find (pid);
  double ps = (itp != shared.scoreByNode.end ()) ? itp->second : 0.0;
  uint32_t bestId = 0;
  double bestSc = -1.0;
  for (const auto& kv : shared.scoreByNode)
    {
      auto itTime = shared.scoreTimeByNode.find (kv.first);
      if (itTime == shared.scoreTimeByNode.end ())
        {
          continue;
        }
      double age = now - itTime->second;
      if (age > m_spnScoreStaleSec)
        {
          continue;
        }
      if (kv.first == pid)
        {
          continue;
        }
      if (kv.second <= ps + SCORE_ELECT_MARGIN)
        {
          continue;
        }
      if (kv.second > bestSc || (kv.second == bestSc && (bestId == 0 || kv.first < bestId)))
        {
          bestSc = kv.second;
          bestId = kv.first;
        }
    }
  if (bestId == 0)
    {
      shared.challengeChallengerId = 0;
      shared.challengeConsecutive = 0;
      return;
    }
  if (bestId == shared.challengeChallengerId)
    {
      shared.challengeConsecutive++;
    }
  else
    {
      shared.challengeChallengerId = bestId;
      shared.challengeConsecutive = 1;
    }
  if (shared.challengeConsecutive >= CONDITION_C_CONFIRM_PERIODS)
    {
      bool intervalOk =
          (shared.lastElectTs < 0.0) || (now - shared.lastElectTs >= MIN_ELECT_INTERVAL_SEC);
      if (intervalOk)
        {
          RunFullElection ("condition_c_periodic_score_reselect");
        }
    }
}

void
HeterogeneousNodeApp::OnConditionCEval ()
{
  if (!m_running)
    {
      return;
    }
  if (m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK)
    {
      return;
    }
  uint32_t selfId = GetNode ()->GetId ();
  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
  if (!shared.initialized || selfId != shared.primaryId)
    {
      return;
    }
  double myScore = CalculateUtilityScore ();
  SpnMergeScoresIntoShared (myScore);
  EvaluateConditionC ();
  if (m_running && selfId == shared.primaryId)
    {
      m_conditionCTimer = Simulator::Schedule (Seconds (CONDITION_C_PERIOD_SEC),
                                              &HeterogeneousNodeApp::OnConditionCEval, this);
    }
}

void
HeterogeneousNodeApp::SyncConditionCTimer ()
{
  if (m_subnetType != SUBNET_ADHOC && m_subnetType != SUBNET_DATALINK)
    {
      return;
    }
  if (m_conditionCTimer.IsRunning ())
    {
      Simulator::Cancel (m_conditionCTimer);
    }
  if (!m_running)
    {
      return;
    }
  SharedSpnState& shared = s_sharedSpnState[static_cast<uint8_t> (m_subnetType)];
  uint32_t selfId = GetNode ()->GetId ();
  if (shared.initialized && selfId == shared.primaryId)
    {
      m_conditionCTimer = Simulator::Schedule (Seconds (CONDITION_C_PERIOD_SEC),
                                              &HeterogeneousNodeApp::OnConditionCEval, this);
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

  // Adhoc/DataLink：在 join_time 之后 0.1s 触发首轮选举（SPN_ANNOUNCE reason=initial_lock，与 LTE 一致）
  if (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK)
    {
      double delay = std::max (0.0, m_joinTime - Simulator::Now ().GetSeconds ()) + 0.1;
      Simulator::Schedule (Seconds (delay), &HeterogeneousNodeApp::RunInitialLockElection, this);
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
  if (m_conditionCTimer.IsRunning ())
    {
      Simulator::Cancel (m_conditionCTimer);
    }
  if (m_chPeriodicEvent.IsRunning ())
    {
      Simulator::Cancel (m_chPeriodicEvent);
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
  NMS_LOG_INFO (nodeId, "NODE_OFFLINE", "Node " + std::to_string (nodeId) + " application stopped (OFFLINE)");
  StopApplication ();
}

void
HeterogeneousNodeApp::TriggerElectionNow (const std::string& electReason)
{
  if (!m_running || !(m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK))
    {
      return;
    }
  RunFullElection (electReason);
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
  // 与 Adhoc 统一为完整 6 字节 HNMP 头（仿真层仍走 UDP/IP；真实数据链可在论文中描述为逻辑等效载荷）
  return Hnmp::EncodeFrame (out, outSize, h, tlv, h.payloadLen, false);
}

bool
HeterogeneousNodeApp::ParseHnmpFrame (const uint8_t* in, uint32_t inLen,
                                      const uint8_t** outPayload, uint32_t* outPayloadLen)
{
  if (!in || inLen == 0 || !outPayload || !outPayloadLen) return false;
  Hnmp::Header h = {};
  if (inLen >= 6)
    {
      uint8_t pLen = in[5];
      if (inLen == 6u + pLen)
        {
          if (Hnmp::DecodeFrame (in, inLen, false, &h, outPayload, outPayloadLen))
            return true;
        }
    }
  if (inLen >= 3)
    {
      uint8_t pLen = in[2];
      if (inLen == 3u + pLen)
        {
          if (Hnmp::DecodeFrame (in, inLen, true, &h, outPayload, outPayloadLen))
            return true;
        }
    }
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

  // 3) Adhoc/DataLink：静默更新分数与本地角色；仅满足条件时触发 RunFullElection（与 STATE_CHANGE 解耦）
  if (m_subnetType == SUBNET_ADHOC || m_subnetType == SUBNET_DATALINK)
    {
      SpnSyncScoresOnly (myScore);
      MaybeTriggerSpnElection (myScore);
      if (!m_initialScoreFloodSent)
        {
          m_initialScoreFloodSent = true;
          Simulator::Schedule (MilliSeconds (100), &HeterogeneousNodeApp::BroadcastScoreFloodCurrent, this);
        }
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
      // 轻量队列占用监控：持续抑制视作发送队列积压增加
      if (decisionReason == "IN_SUPPRESS_WINDOW_SKIP")
        {
          m_queueOccupancyRatio = std::min (1.0, m_queueOccupancyRatio + 0.08);
        }
      else
        {
          m_queueOccupancyRatio = std::min (1.0, m_queueOccupancyRatio + 0.03);
        }
      LogDecision ("SKIP", decisionReason, stateDelta, now);
      ScheduleNextTx ();
      return;
    }
  m_triggeredScheduleDecisions++;
  m_queueOccupancyRatio = std::max (0.0, m_queueOccupancyRatio - 0.20);
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
      m_queueOccupancyRatio = std::max (0.0, m_queueOccupancyRatio - 0.10);
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

/**
 * 将 scenario 中 business_flows.priority 映射为 IPv4 TOS 字节（高 6 位为 DSCP<<2）。
 * ns-3 中 UdpSocket 根据 TOS 计算 SocketPriority，并打上 SocketPriorityTag；PfifoFastQueueDisc
 * 按优先级选择 band（band 0 最先出队）。数值越大（>=2）表示业务越重要。
 */
static uint8_t
MapBusinessPriorityToIpTos (uint8_t businessPriority)
{
  if (businessPriority >= 2)
    {
      return 0x10; // -> NS3_PRIO_INTERACTIVE -> PfifoFast band 0
    }
  if (businessPriority == 1)
    {
      return 0x48; // -> NS3_PRIO_BULK -> band 2
    }
  return 0xb8; // -> NS3_PRIO_INTERACTIVE_BULK -> band 1
}

class HeterogeneousNmsFramework
{
public:
  enum RoutingMode
  {
    ROUTING_AUTO = 0,
    ROUTING_AODV,
    ROUTING_OLSR
  };
  enum RouteAdaptLevel
  {
    ADAPT_AUTO = 0,
    ADAPT_STABLE,
    ADAPT_DEGRADED,
    ADAPT_CRITICAL
  };
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
  void SetRoutingMode (const std::string& mode);
  void SetRouteAdaptLevel (const std::string& level);
  void SetRouteAdaptRuntime (bool enable) { m_routeAdaptRuntimeEnable = enable; }
  void SetRouteAdaptRuntimeWindowSec (double v) { if (v > 0.1) m_routeAdaptRuntimeWindowSec = v; }
  void SetRouteAdaptRuntimeCooldownSec (double v) { if (v > 0.1) m_routeAdaptRuntimeCooldownSec = v; }
  /// InstallApplications 前：由 config.json 注入 SPN 选举超时与心跳丢失阈值（默认与构造函数一致；scenario_config 仍可在 Run 内覆盖）
  void SetFrameworkSpnElectionTunables (double electionTimeoutSec, uint8_t heartbeatMissThreshold);

private:
  // === 分步骤方法 ===
  void BuildGmc ();              ///< 构建全局管理中心节点
  void BuildLteSubnet ();        ///< 构建子网 A：蜂窝接入网 (LTE)
  void BuildAdhocSubnet ();      ///< 构建子网 B：WiFi 自组网 + OLSR
  void BuildDataLinkSubnet ();   ///< 构建子网 C：窄带数据链 (11a 低速率模拟)
  void SetupBackhaul ();         ///< 为每个 SPN 与 GMC 搭建有线 P2P 回程链路
  void ElectInitialSpn ();      ///< 按性能评分选举各子网 SPN（LTE 固定 eNB，Adhoc/DataLink 选举）
  /// LTE 不参与 RunElection，单独输出 SPN_ANNOUNCE 供可视化解析主/备与 GMC 回程
  void EmitLteSpnAnnounce ();
  void InstallApplications ();   ///< 在 GMC 与各 SPN 安装自定义应用
  void SetupMonitoring ();       ///< 配置 FlowMonitor 和 NetAnim
  /// 周期性输出业务流窗口性能（FLOW_PERF_WIN），用于前端真实时间序列
  void EmitFlowPerformanceWindow ();
  /// 为可视化输出：写当前时刻所有节点状态到 JSONL，并调度下一次（每 0.5s）
  void WriteAllNodesJsonl ();
  /// 时序入网：t=0 时将 joinTime>0 的节点接口设为 Down，joinTime 时刻再 BringUp
  void BringDownNodesWithDelayedJoin ();
  static void BringUpNode (Ptr<Node> node);
  /// 事件注入：统一退网（NODE_OFFLINE），reasonKey 为 fault|voluntary；GMC 会被忽略
  /// systemReason：ENERGY_DEPLETED / LINK_QUALITY_DEGRADED / DIRECT_FAIL（空则不打 SYSTEM 行）
  void InjectNodeOffline (uint32_t nodeId, std::string reasonKey,
                          const std::string& systemReason = std::string (),
                          double logEnergy = -1.0, double logLinkQ = -1.0, double logThreshold = -1.0);
  void ScenarioInjectEnergy (uint32_t nodeId, double energy);
  void ScenarioInjectLinkQuality (uint32_t nodeId, double linkQ);
  void OnPhysicsMonitorTick ();
  void LogMemberNodeLostOnSubnetPrimary (uint32_t lostNodeId);
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
  /// 业务 UDP 流 QoS：在 WiFi/LTE UE 网卡上安装 PfifoFast，并按 priority 设置 IP TOS（映射到 SocketPriority / DSCP）
  void InstallBusinessFlowQosQueueDiscs ();
  /// 通过 OLSR 路由表解析 src -> dst 的逐跳路径（仅 Adhoc 子网）
  std::vector<uint32_t> GetOlsrPath (uint32_t srcNodeId, uint32_t dstNodeId) const;
  /// 根据节点 ID 获取 Ptr<Node>（遍历 NodeList）
  static Ptr<Node> GetNodeById (uint32_t nodeId);
  RoutingMode DecideRoutingModeAuto (const ScenarioConfig* sc) const;
  static std::string RoutingModeToString (RoutingMode mode);
  RouteAdaptLevel DecideRouteAdaptLevelAuto (const ScenarioConfig* sc) const;
  static std::string RouteAdaptLevelToString (RouteAdaptLevel level);
  void ApplyAdhocRoutingAdaptiveParams ();
  void ApplyOlsrRuntimeParamsToAdhoc (double helloSec, double tcSec);
  void MaybeUpdateRuntimeRouteAdapt (double avgLossPct, double avgDelayMs, uint32_t zeroThroughputFlows, double nowSec);

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
  RoutingMode         m_routingModeConfigured; ///< 命令行/上层指定的路由模式（auto/aodv/olsr）
  RoutingMode         m_routingModeEffective;  ///< 本轮仿真最终生效的路由模式
  RouteAdaptLevel     m_routeAdaptConfigured;  ///< 路由参数自适应档位（auto/stable/degraded/critical）
  RouteAdaptLevel     m_routeAdaptEffective;   ///< 本轮仿真生效档位
  double              m_adhocOlsrHelloSec;     ///< Adhoc OLSR HelloInterval
  double              m_adhocOlsrTcSec;        ///< Adhoc OLSR TcInterval
  double              m_adhocAodvHelloSec;     ///< Adhoc AODV HelloInterval
  double              m_adhocAodvActiveRouteTimeoutSec; ///< Adhoc AODV ActiveRouteTimeout
  uint32_t            m_adhocAodvAllowedHelloLoss; ///< Adhoc AODV AllowedHelloLoss
  bool                m_routeAdaptRuntimeEnable; ///< 运行时路由参数调优开关（默认关闭，不影响既有行为）
  double              m_routeAdaptRuntimeWindowSec; ///< 运行时评估窗口（秒）
  double              m_routeAdaptRuntimeCooldownSec; ///< 运行时档位切换冷却时间（秒）
  double              m_routeAdaptRuntimeLastSwitchTs; ///< 最近一次切换时刻（秒）
  double              m_routeAdaptRuntimeLastEvalTs; ///< 最近一次运行时评估时刻（秒）
  uint32_t            m_routeAdaptUpgradeVotes; ///< 升档确认计数
  uint32_t            m_routeAdaptDowngradeVotes; ///< 降档确认计数
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

  /// NODE_ENERGY_FAIL / LINK_INTERFERENCE_FAIL：周期监测（每 1s）
  enum class PhysicsWatchKind
  {
    ENERGY,
    LINK
  };
  struct PhysicsWatchEntry
  {
    uint32_t target;
    PhysicsWatchKind kind;
    double threshold;
    bool done;
  };
  std::vector<PhysicsWatchEntry> m_physicsWatches;
  std::map<uint32_t, std::string> m_nodeOfflineSystemReason; ///< JSONL offline_reason

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
    m_simTimeSeconds (90.0),
    m_adhocTopology (ADHOC_TOPOLOGY_MESH),
    m_enablePacketParse (true),
    m_enablePcap (false),
    m_useDualChannel (false),
    m_energyDeltaThreshold (0.15),
    m_stateSuppressWindowSec (15.0),
    m_aggregateIntervalSec (2.0),
    m_scenarioMode ("normal"),
    m_routingModeConfigured (ROUTING_AUTO),
    m_routingModeEffective (ROUTING_OLSR),
    m_routeAdaptConfigured (ADAPT_AUTO),
    m_routeAdaptEffective (ADAPT_STABLE),
    m_adhocOlsrHelloSec (0.5),
    m_adhocOlsrTcSec (1.0),
    m_adhocAodvHelloSec (0.5),
    m_adhocAodvActiveRouteTimeoutSec (4.0),
    m_adhocAodvAllowedHelloLoss (3),
    m_routeAdaptRuntimeEnable (false),
    m_routeAdaptRuntimeWindowSec (5.0),
    m_routeAdaptRuntimeCooldownSec (20.0),
    m_routeAdaptRuntimeLastSwitchTs (-1.0),
    m_routeAdaptRuntimeLastEvalTs (-1.0),
    m_routeAdaptUpgradeVotes (0),
    m_routeAdaptDowngradeVotes (0),
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

void
HeterogeneousNmsFramework::SetRoutingMode (const std::string& mode)
{
  if (mode == "aodv" || mode == "AODV")
    {
      m_routingModeConfigured = ROUTING_AODV;
      return;
    }
  if (mode == "olsr" || mode == "OLSR")
    {
      m_routingModeConfigured = ROUTING_OLSR;
      return;
    }
  m_routingModeConfigured = ROUTING_AUTO;
}

void
HeterogeneousNmsFramework::SetRouteAdaptLevel (const std::string& level)
{
  if (level == "stable" || level == "STABLE")
    {
      m_routeAdaptConfigured = ADAPT_STABLE;
      return;
    }
  if (level == "degraded" || level == "DEGRADED")
    {
      m_routeAdaptConfigured = ADAPT_DEGRADED;
      return;
    }
  if (level == "critical" || level == "CRITICAL")
    {
      m_routeAdaptConfigured = ADAPT_CRITICAL;
      return;
    }
  m_routeAdaptConfigured = ADAPT_AUTO;
}

std::string
HeterogeneousNmsFramework::RoutingModeToString (RoutingMode mode)
{
  if (mode == ROUTING_AODV)
    {
      return "aodv";
    }
  if (mode == ROUTING_OLSR)
    {
      return "olsr";
    }
  return "auto";
}

std::string
HeterogeneousNmsFramework::RouteAdaptLevelToString (RouteAdaptLevel level)
{
  if (level == ADAPT_STABLE)
    {
      return "stable";
    }
  if (level == ADAPT_DEGRADED)
    {
      return "degraded";
    }
  if (level == ADAPT_CRITICAL)
    {
      return "critical";
    }
  return "auto";
}

HeterogeneousNmsFramework::RoutingMode
HeterogeneousNmsFramework::DecideRoutingModeAuto (const ScenarioConfig* sc) const
{
  // 管理策略（方案1）：每轮仿真启动前一次性决策，不做运行时热切换
  // 经验规则：持续并发业务/高负载场景更偏向 OLSR；轻载/基线场景偏向 AODV 以降低控制开销。
  if (m_scenarioMode == "compare-baseline")
    {
      return ROUTING_AODV;
    }
  if (m_scenarioMode == "compare-hnmp" || m_scenarioMode == "thesis30")
    {
      return ROUTING_OLSR;
    }
  if (sc && !sc->businessFlows.empty ())
    {
      double totalOfferedMbps = 0.0;
      auto parseRateMbps = [] (const std::string& s) -> double {
        if (s.empty ())
          {
            return 0.0;
          }
        std::string lower = s;
        std::transform (lower.begin (), lower.end (), lower.begin (), ::tolower);
        char* endp = nullptr;
        double val = std::strtod (lower.c_str (), &endp);
        if (!std::isfinite (val) || endp == lower.c_str ())
          {
            return 0.0;
          }
        std::string unit = lower.substr (static_cast<size_t> (endp - lower.c_str ()));
        if (unit.find ("gbps") != std::string::npos)
          {
            return val * 1000.0;
          }
        if (unit.find ("kbps") != std::string::npos)
          {
            return val / 1000.0;
          }
        return val;
      };
      for (const auto& bf : sc->businessFlows)
        {
          totalOfferedMbps += parseRateMbps (bf.dataRate);
        }
      if (sc->businessFlows.size () >= 4 || totalOfferedMbps >= 3.0)
        {
          return ROUTING_OLSR;
        }
    }
  return ROUTING_AODV;
}

HeterogeneousNmsFramework::RouteAdaptLevel
HeterogeneousNmsFramework::DecideRouteAdaptLevelAuto (const ScenarioConfig* sc) const
{
  if (m_scenarioMode == "thesis30")
    {
      return ADAPT_CRITICAL;
    }
  if (m_scenarioMode == "compare-hnmp")
    {
      return ADAPT_DEGRADED;
    }
  if (sc && !sc->events.empty ())
    {
      if (sc->events.size () >= 3)
        {
          return ADAPT_CRITICAL;
        }
      return ADAPT_DEGRADED;
    }
  return ADAPT_STABLE;
}

void
HeterogeneousNmsFramework::ApplyAdhocRoutingAdaptiveParams ()
{
  if (m_routeAdaptEffective == ADAPT_CRITICAL)
    {
      m_adhocOlsrHelloSec = 0.25;
      m_adhocOlsrTcSec = 0.5;
      m_adhocAodvHelloSec = 0.25;
      m_adhocAodvActiveRouteTimeoutSec = 3.0;
      m_adhocAodvAllowedHelloLoss = 2;
      return;
    }
  if (m_routeAdaptEffective == ADAPT_DEGRADED)
    {
      m_adhocOlsrHelloSec = 0.5;
      m_adhocOlsrTcSec = 1.0;
      m_adhocAodvHelloSec = 0.5;
      m_adhocAodvActiveRouteTimeoutSec = 4.0;
      m_adhocAodvAllowedHelloLoss = 3;
      return;
    }
  m_adhocOlsrHelloSec = 1.0;
  m_adhocOlsrTcSec = 2.0;
  m_adhocAodvHelloSec = 1.0;
  m_adhocAodvActiveRouteTimeoutSec = 6.0;
  m_adhocAodvAllowedHelloLoss = 4;
}

void
HeterogeneousNmsFramework::ApplyOlsrRuntimeParamsToAdhoc (double helloSec, double tcSec)
{
  for (uint32_t i = 0; i < m_adhocNodes.GetN (); ++i)
    {
      Ptr<Node> n = m_adhocNodes.Get (i);
      Ptr<Ipv4> ipv4 = n->GetObject<Ipv4> ();
      if (!ipv4)
        {
          continue;
        }
      Ptr<Ipv4ListRouting> list = DynamicCast<Ipv4ListRouting> (ipv4->GetRoutingProtocol ());
      if (!list)
        {
          continue;
        }
      for (uint32_t idx = 0; idx < list->GetNRoutingProtocols (); ++idx)
        {
          int16_t pri = 0;
          Ptr<Ipv4RoutingProtocol> rp = list->GetRoutingProtocol (idx, pri);
          Ptr<olsr::RoutingProtocol> olsrRp = DynamicCast<olsr::RoutingProtocol> (rp);
          if (!olsrRp)
            {
              continue;
            }
          olsrRp->SetAttribute ("HelloInterval", TimeValue (Seconds (helloSec)));
          olsrRp->SetAttribute ("TcInterval", TimeValue (Seconds (tcSec)));
          olsrRp->SetAttribute ("MidInterval", TimeValue (Seconds (std::max (1.0, tcSec * 2.0))));
          olsrRp->SetAttribute ("HnaInterval", TimeValue (Seconds (std::max (1.0, tcSec * 2.0))));
        }
    }
}

void
HeterogeneousNmsFramework::MaybeUpdateRuntimeRouteAdapt (double avgLossPct,
                                                         double avgDelayMs,
                                                         uint32_t zeroThroughputFlows,
                                                         double nowSec)
{
  if (!m_routeAdaptRuntimeEnable || m_routingModeEffective != ROUTING_OLSR)
    {
      return;
    }
  if (m_routeAdaptRuntimeLastSwitchTs >= 0.0 &&
      nowSec - m_routeAdaptRuntimeLastSwitchTs < m_routeAdaptRuntimeCooldownSec)
    {
      return;
    }
  RouteAdaptLevel target = m_routeAdaptEffective;
  if (zeroThroughputFlows >= 2 || avgLossPct >= 5.0)
    {
      target = ADAPT_CRITICAL;
    }
  else if (zeroThroughputFlows >= 1 || avgLossPct >= 1.5 || avgDelayMs >= 25.0)
    {
      target = ADAPT_DEGRADED;
    }
  else
    {
      target = ADAPT_STABLE;
    }

  if (target > m_routeAdaptEffective)
    {
      m_routeAdaptUpgradeVotes++;
      m_routeAdaptDowngradeVotes = 0;
      if (m_routeAdaptUpgradeVotes < 2)
        {
          return;
        }
    }
  else if (target < m_routeAdaptEffective)
    {
      m_routeAdaptDowngradeVotes++;
      m_routeAdaptUpgradeVotes = 0;
      if (m_routeAdaptDowngradeVotes < 3)
        {
          return;
        }
    }
  else
    {
      m_routeAdaptUpgradeVotes = 0;
      m_routeAdaptDowngradeVotes = 0;
      return;
    }

  RouteAdaptLevel oldLevel = m_routeAdaptEffective;
  m_routeAdaptEffective = target;
  ApplyAdhocRoutingAdaptiveParams ();
  ApplyOlsrRuntimeParamsToAdhoc (m_adhocOlsrHelloSec, m_adhocOlsrTcSec);
  m_routeAdaptRuntimeLastSwitchTs = nowSec;
  m_routeAdaptUpgradeVotes = 0;
  m_routeAdaptDowngradeVotes = 0;
  NmsLog ("INFO", 0, "ROUTE_ADAPT_RUNTIME_APPLY",
          "old=" + RouteAdaptLevelToString (oldLevel) +
          " new=" + RouteAdaptLevelToString (m_routeAdaptEffective) +
          " olsr_hello=" + std::to_string (m_adhocOlsrHelloSec) +
          " olsr_tc=" + std::to_string (m_adhocOlsrTcSec));
}

void
HeterogeneousNmsFramework::SetFrameworkSpnElectionTunables (double electionTimeoutSec, uint8_t heartbeatMissThreshold)
{
  if (electionTimeoutSec > 0.0)
    {
      m_spnElectionTimeoutSec = electionTimeoutSec;
    }
  if (heartbeatMissThreshold >= 1u)
    {
      m_spnHeartbeatMissThreshold = heartbeatMissThreshold;
    }
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

namespace {
std::string
ToLowerCopy (const std::string& s)
{
  std::string r = s;
  for (char& c : r)
    c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
  return r;
}

const char*
OfflineReasonKeyToZh (const std::string& rk)
{
  if (rk == "voluntary")
    return "主动退网";
  if (rk == "power_low" || rk == "battery_low" || rk == "energy_low")
    return "电量过低";
  if (rk == "link_loss" || rk == "link_interference" || rk == "interference" || rk == "link_break"
      || rk == "link_down")
    return "链路干扰/断链";
  if (rk == "fault")
    return "节点故障";
  return "节点故障";
}
} // namespace

void
HeterogeneousNmsFramework::LogMemberNodeLostOnSubnetPrimary (uint32_t lostNodeId)
{
  auto itLost = m_joinConfig.find (lostNodeId);
  if (itLost == m_joinConfig.end ())
    {
      return;
    }
  const std::string sub = itLost->second.subnet;
  uint32_t logOn = std::numeric_limits<uint32_t>::max ();
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      uint32_t nid = (*it)->GetId ();
      auto j = m_joinConfig.find (nid);
      if (j == m_joinConfig.end () || j->second.subnet != sub)
        {
          continue;
        }
      for (uint32_t i = 0; i < (*it)->GetNApplications (); ++i)
        {
          Ptr<HeterogeneousNodeApp> ap = DynamicCast<HeterogeneousNodeApp> ((*it)->GetApplication (i));
          if (ap && ap->IsSpn () && !ap->IsBackupSpn ())
            {
              logOn = nid;
              break;
            }
        }
      if (logOn != std::numeric_limits<uint32_t>::max ())
        {
          break;
        }
    }
  if (logOn == std::numeric_limits<uint32_t>::max ())
    {
      for (const auto& kv : m_joinConfig)
        {
          if (kv.second.subnet != sub)
            {
              continue;
            }
          std::string tp = kv.second.type;
          for (char& c : tp)
            c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
          if (tp == "enb")
            {
              logOn = kv.first;
              break;
            }
        }
    }
  if (logOn != std::numeric_limits<uint32_t>::max ())
    {
      NmsLog ("INFO", logOn, "SPN",
              std::string ("member_node_lost node=") + std::to_string (lostNodeId));
    }
}

void
HeterogeneousNmsFramework::ScenarioInjectEnergy (uint32_t nodeId, double energy)
{
  Ptr<Node> node = GetNodeById (nodeId);
  if (!node)
    {
      return;
    }
  for (uint32_t i = 0; i < node->GetNApplications (); ++i)
    {
      Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> (node->GetApplication (i));
      if (app)
        {
          app->ApplyScenarioInjectEnergy (energy);
          std::ostringstream o;
          o << "injected_energy=" << std::fixed << std::setprecision (2) << energy;
          NmsLog ("INFO", nodeId, "SCENARIO_INJECT", o.str ());
          return;
        }
    }
}

void
HeterogeneousNmsFramework::ScenarioInjectLinkQuality (uint32_t nodeId, double linkQ)
{
  Ptr<Node> node = GetNodeById (nodeId);
  if (!node)
    {
      return;
    }
  for (uint32_t i = 0; i < node->GetNApplications (); ++i)
    {
      Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> (node->GetApplication (i));
      if (app)
        {
          app->ApplyScenarioInjectLinkQuality (linkQ);
          std::ostringstream o;
          o << "injected_link_quality=" << std::fixed << std::setprecision (2) << linkQ;
          NmsLog ("INFO", nodeId, "SCENARIO_INJECT", o.str ());
          return;
        }
    }
}

void
HeterogeneousNmsFramework::OnPhysicsMonitorTick ()
{
  double now = Simulator::Now ().GetSeconds ();
  for (auto& w : m_physicsWatches)
    {
      if (w.done)
        {
          continue;
        }
      Ptr<Node> n = GetNodeById (w.target);
      if (!n)
        {
          w.done = true;
          continue;
        }
      Ptr<HeterogeneousNodeApp> hap;
      for (uint32_t i = 0; i < n->GetNApplications (); ++i)
        {
          hap = DynamicCast<HeterogeneousNodeApp> (n->GetApplication (i));
          if (hap)
            {
              break;
            }
        }
      if (!hap || !hap->IsApplicationRunning ())
        {
          continue;
        }
      const double e = hap->GetUvMibEnergy ();
      const double lq = hap->GetUvMibLinkQuality ();
      bool trip = false;
      if (w.kind == PhysicsWatchKind::ENERGY && e < w.threshold)
        {
          trip = true;
        }
      if (w.kind == PhysicsWatchKind::LINK && lq < w.threshold)
        {
          trip = true;
        }
      if (!trip)
        {
          continue;
        }
      w.done = true;
      if (w.kind == PhysicsWatchKind::ENERGY)
        {
          std::ostringstream ot;
          ot << "reason=ENERGY_BELOW_THRESHOLD energy=" << std::fixed << std::setprecision (2) << e
             << " threshold=" << std::setprecision (2) << w.threshold;
          NmsLog ("INFO", w.target, "EVENT_TRIGGER", ot.str ());
          InjectNodeOffline (w.target, "power_low", "ENERGY_DEPLETED", e, -1.0, w.threshold);
        }
      else
        {
          std::ostringstream ot;
          ot << "linkQ=" << std::fixed << std::setprecision (2) << lq << " threshold=" << std::setprecision (2)
             << w.threshold << " → TRIGGERING NODE_FAIL";
          NmsLog ("INFO", w.target, "LINK_DEGRADE", ot.str ());
          InjectNodeOffline (w.target, "link_loss", "LINK_QUALITY_DEGRADED", -1.0, lq, w.threshold);
        }
    }
  if (now + 1.0 <= m_simTimeSeconds)
    {
      Simulator::Schedule (Seconds (1.0), &HeterogeneousNmsFramework::OnPhysicsMonitorTick, this);
    }
}

void
HeterogeneousNmsFramework::InjectNodeOffline (uint32_t nodeId, std::string reasonKey,
                                              const std::string& systemReason,
                                              double logEnergy, double logLinkQ, double logThreshold)
{
  (void) logThreshold;
  uint32_t gmcId = m_gmcNode.Get (0)->GetId ();
  if (nodeId == gmcId)
    {
      NmsLog ("WARN", 0, "SYSTEM", "NODE_OFFLINE ignored: target is GMC (GMC cannot go offline)");
      return;
    }
  std::string rk = ToLowerCopy (reasonKey);
  if (rk.empty ())
    rk = "fault";
  Ptr<Node> node = GetNodeById (nodeId);
  if (!node)
    {
      NmsLog ("WARN", 0, "NODE_OFFLINE", "InjectNodeOffline: node " + std::to_string (nodeId) + " not found");
      return;
    }
  Ptr<HeterogeneousNodeApp> targetApp;
  for (uint32_t i = 0; i < node->GetNApplications (); ++i)
    {
      targetApp = DynamicCast<HeterogeneousNodeApp> (node->GetApplication (i));
      if (targetApp)
        {
          break;
        }
    }
  bool triggerFullReselect = false;
  if (targetApp)
    {
      HeterogeneousNodeApp::SubnetType st = targetApp->GetSubnetType ();
      if (st == HeterogeneousNodeApp::SUBNET_ADHOC || st == HeterogeneousNodeApp::SUBNET_DATALINK)
        {
          triggerFullReselect = targetApp->IsSpn () || targetApp->IsBackupSpn ();
        }
    }
  if (!systemReason.empty ())
    {
      m_nodeOfflineSystemReason[nodeId] = systemReason;
      std::ostringstream sys;
      sys << "Triggered NODE_FAIL on Node " << nodeId << " reason=" << systemReason;
      if (systemReason == "ENERGY_DEPLETED" && logEnergy >= 0.0)
        {
          sys << " energy=" << std::fixed << std::setprecision (2) << logEnergy;
        }
      if (systemReason == "LINK_QUALITY_DEGRADED" && logLinkQ >= 0.0)
        {
          sys << " linkQ=" << std::fixed << std::setprecision (2) << logLinkQ;
        }
      NmsLog ("INFO", 0, "SYSTEM", sys.str ());
    }
  const char* reasonZh = OfflineReasonKeyToZh (rk);
  std::string subnetLbl = "—";
  std::string roleLbl = "—";
  auto itJ = m_joinConfig.find (nodeId);
  if (itJ != m_joinConfig.end ())
    {
      subnetLbl = itJ->second.subnet;
      std::string tp = itJ->second.type;
      if (tp == "enb") roleLbl = "SPN";
      else if (tp == "ue") roleLbl = "UE";
      else if (tp == "gmc") roleLbl = "GMC";
      else roleLbl = "TSN";
    }
  uint32_t rem = 0;
  if (itJ != m_joinConfig.end ())
    {
      const std::string sub = itJ->second.subnet;
      for (const auto& kv : m_joinConfig)
        {
          if (kv.second.subnet == sub && kv.first != nodeId && kv.first != gmcId)
            {
              ++rem;
            }
        }
    }
  double tnow = Simulator::Now ().GetSeconds ();
  {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision (1) << "t=" << tnow << "s Node " << nodeId << " 退网 (原因: " << reasonZh << ")";
    NmsLog ("INFO", nodeId, "NODE_OFFLINE", oss.str ());
  }
  {
    std::ostringstream oss2;
    oss2 << "所属子网: " << subnetLbl << ", 原角色: " << roleLbl << ", 剩余在网: " << rem;
    NmsLog ("INFO", nodeId, "NODE_OFFLINE", oss2.str ());
  }
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
  for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
    {
      if ((*it)->GetId () == nodeId) continue;
      for (uint32_t i = 0; i < (*it)->GetNApplications (); ++i)
        {
          Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> ((*it)->GetApplication (i));
          if (app)
            {
              app->SpnForgetPeer (nodeId);
              break;
            }
        }
    }
  if (triggerFullReselect)
    {
      for (NodeList::Iterator it = NodeList::Begin (); it != NodeList::End (); ++it)
        {
          if ((*it)->GetId () == nodeId) continue;
          for (uint32_t i = 0; i < (*it)->GetNApplications (); ++i)
            {
              Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> ((*it)->GetApplication (i));
              if (app)
                {
                  app->TriggerElectionNow ("spn_peer_offline_reselect");
                  break;
                }
            }
        }
    }
  else
    {
      LogMemberNodeLostOnSubnetPrimary (nodeId);
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
  mobilityEnb.SetMobilityModel ("ns3::ConstantVelocityMobilityModel");
  mobilityEnb.Install (m_lteEnbNodes);
  Ptr<ConstantVelocityMobilityModel> enbMob =
      m_lteEnbNodes.Get (0)->GetObject<ConstantVelocityMobilityModel> ();
  if (enbMob)
    {
      double vxEnb = 0.0;
      if (itEnb != m_joinConfig.end () && itEnb->second.speed > 0.0)
        {
          vxEnb = itEnb->second.speed;
        }
      enbMob->SetVelocity (Vector (vxEnb, 0.0, 0.0));
    }

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
  mobilityUe.SetMobilityModel ("ns3::ConstantVelocityMobilityModel");
  mobilityUe.Install (m_lteUeNodes);
  for (uint32_t i = 0; i < m_lteUeNodes.GetN (); ++i)
    {
      Ptr<ConstantVelocityMobilityModel> m =
          m_lteUeNodes.Get (i)->GetObject<ConstantVelocityMobilityModel> ();
      if (!m)
        {
          continue;
        }
      double vx = 8.0;
      auto itUe = m_joinConfig.find (m_lteUeNodes.Get (i)->GetId ());
      if (itUe != m_joinConfig.end () && itUe->second.speed > 0.0)
        {
          vx = itUe->second.speed;
        }
      m->SetVelocity (Vector (vx, 0.0, 0.0));
    }
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
  olsr.Set ("HelloInterval", TimeValue (Seconds (m_adhocOlsrHelloSec)));
  olsr.Set ("TcInterval", TimeValue (Seconds (m_adhocOlsrTcSec)));
  olsr.Set ("MidInterval", TimeValue (Seconds (std::max (1.0, m_adhocOlsrTcSec * 2.0))));
  olsr.Set ("HnaInterval", TimeValue (Seconds (std::max (1.0, m_adhocOlsrTcSec * 2.0))));
  AodvHelper aodv;
  aodv.Set ("HelloInterval", TimeValue (Seconds (m_adhocAodvHelloSec)));
  aodv.Set ("ActiveRouteTimeout", TimeValue (Seconds (m_adhocAodvActiveRouteTimeoutSec)));
  aodv.Set ("AllowedHelloLoss", UintegerValue (m_adhocAodvAllowedHelloLoss));
  Ipv4StaticRoutingHelper staticRouting;
  Ipv4ListRoutingHelper list;
  list.Add (staticRouting, 30);
  if (m_routingModeEffective == ROUTING_AODV)
    {
      list.Add (aodv, 20);
    }
  else
    {
      list.Add (olsr, 20);
    }
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
  // 完全由 JSON 驱动：位置与速度均由 join_config 驱动（未配置速度时用默认值）
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
  mobility.SetMobilityModel ("ns3::ConstantVelocityMobilityModel");
  mobility.Install (m_adhocNodes);
  for (uint32_t i = 0; i < n; ++i)
    {
      Ptr<ConstantVelocityMobilityModel> m =
          m_adhocNodes.Get (i)->GetObject<ConstantVelocityMobilityModel> ();
      if (!m)
        {
          continue;
        }
      double vx = 8.0;
      auto it = m_joinConfig.find (m_adhocNodes.Get (i)->GetId ());
      if (it != m_joinConfig.end () && it->second.speed > 0.0)
        {
          vx = it->second.speed;
        }
      m->SetVelocity (Vector (vx, 0.0, 0.0));
    }
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

  Ipv4StaticRoutingHelper staticRouting;
  Ipv4ListRoutingHelper list;
  list.Add (staticRouting, 30);
  // DataLink 子网按“民用数据链”建模：不启用网络层动态路由（AODV/OLSR），仅使用静态/受管转发。
  InternetStackHelper stack;
  stack.SetRoutingHelper (list);
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

void
HeterogeneousNmsFramework::InstallBusinessFlowQosQueueDiscs ()
{
  NetDeviceContainer qosDevs;
  if (m_adhocDevs.GetN () > 0)
    {
      qosDevs.Add (m_adhocDevs);
    }
  if (m_datalinkDevs.GetN () > 0)
    {
      qosDevs.Add (m_datalinkDevs);
    }
  if (m_lteUeDevs.GetN () > 0)
    {
      qosDevs.Add (m_lteUeDevs);
    }
  if (qosDevs.GetN () == 0)
    {
      return;
    }
  for (uint32_t i = 0; i < qosDevs.GetN (); ++i)
    {
      Ptr<NetDevice> dev = qosDevs.Get (i);
      Ptr<TrafficControlLayer> tc = dev->GetNode ()->GetObject<TrafficControlLayer> ();
      if (tc && tc->GetRootQueueDiscOnDevice (dev))
        {
          tc->DeleteRootQueueDiscOnDevice (dev);
        }
    }
  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::PfifoFastQueueDisc",
                        "MaxSize", QueueSizeValue (QueueSize ("1000p")));
  tch.Install (qosDevs);
  NmsLog ("INFO", 0, "QOS_TC",
          "PfifoFastQueueDisc installed on " + std::to_string (qosDevs.GetN ())
              + " devices (Adhoc/DataLink/LTE-UE wireless stacks).");
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
HeterogeneousNmsFramework::EmitLteSpnAnnounce ()
{
  if (!m_spnLte)
    {
      return;
    }
  const uint32_t primary = m_spnLte->GetId ();
  // 蜂窝侧无独立备 SPN：backup 与 primary 同 id（与动态子网在无备节点时的处理一致）
  const uint32_t backup = primary;
  std::ostringstream oss;
  oss << "subnet=" << static_cast<uint32_t> (HeterogeneousNodeApp::SUBNET_LTE)
      << " primary=" << primary
      << " backup=" << backup
      << " reason=initial_lock";
  NmsLog ("INFO", primary, "SPN_ANNOUNCE", oss.str ());
  for (uint32_t i = 0; i < m_lteUeNodes.GetN (); ++i)
    {
      uint32_t uid = m_lteUeNodes.Get (i)->GetId ();
      NmsLog ("INFO", uid, "ROLE_ASSIGN", "role=TSN subnet=0");
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

  {
    uint32_t adhoc0 = 0;
    for (uint32_t i = 0; i < m_adhocNodes.GetN (); ++i)
      {
        if (GetNodeJoinTime (m_adhocNodes.Get (i)->GetId ()) <= 0.0)
          adhoc0++;
      }
    uint32_t dl0 = 0;
    for (uint32_t i = 0; i < m_datalinkNodes.GetN (); ++i)
      {
        if (GetNodeJoinTime (m_datalinkNodes.Get (i)->GetId ()) <= 0.0)
          dl0++;
      }
    HeterogeneousNodeApp::SetExpectedInitialElectionMembers (HeterogeneousNodeApp::SUBNET_ADHOC,
                                                            std::max (1u, adhoc0));
    HeterogeneousNodeApp::SetExpectedInitialElectionMembers (HeterogeneousNodeApp::SUBNET_DATALINK,
                                                            std::max (1u, dl0));
  }

  auto installOne = [&] (Ptr<Node> node, Ipv4Address peerAddr, uint16_t peerPort,
                          HeterogeneousNodeApp::SubnetType st, uint32_t size, Time interval,
                          bool isSpn, Ipv4Address subnetBroadcast, const std::string& nodeOnlineMsg,
                          Ipv4Address gmcBackhaulAddr = Ipv4Address ("0.0.0.0"), uint16_t gmcBackhaulPort = 0,
                          bool lteIsEnb = false)
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
    // 静态算力优先读取 join_config；未配置时回退到原默认规则
    double staticCap = -1.0;
    if (itJoin != m_joinConfig.end () && itJoin->second.staticComputeCapability > 0.0)
      {
        staticCap = itJoin->second.staticComputeCapability;
      }
    else
      {
        staticCap = 0.65 + 0.05 * static_cast<double> (node->GetId () % 4);
        if (GetNodeJoinTime (node->GetId ()) <= 0.0)
          {
            staticCap = 0.85 + 0.05 * static_cast<double> (node->GetId () % 3);
          }
      }
    app->SetStaticComputeCapability (std::min (1.0, std::max (0.5, staticCap)));
    if (gmcBackhaulPort != 0)
      app->SetGmcBackhaul (gmcBackhaulAddr, gmcBackhaulPort);
    if (st == HeterogeneousNodeApp::SUBNET_LTE)
      {
        app->SetLteNodeKind (lteIsEnb);
        // 与 Adhoc/DataLink 一致：能量为 [0,1] 归一化量，不再使用 *100
        app->SetInitialUvMib (1.0, 0.9);
      }
    if (itJoin != m_joinConfig.end ())
      {
        app->SetJoinTime (startSec);
        double e = itJoin->second.initialEnergy >= 0.0 ? itJoin->second.initialEnergy : -1.0;
        double lq = itJoin->second.initialLinkQuality >= 0.0 ? itJoin->second.initialLinkQuality : -1.0;
        if (e >= 0.0 || lq >= 0.0)
          app->SetInitialUvMib (e >= 0.0 ? e : 0.9,
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
              false, Ipv4Address ("0.0.0.0"), "LTE SPN (eNB) application started.", Ipv4Address ("0.0.0.0"), 0, true);
  for (uint32_t i = 0; i < m_lteUeNodes.GetN (); ++i)
    {
      Ptr<Node> ue = m_lteUeNodes.Get (i);
      installOne (ue, gmcToLteSpnAddr, gmcDataPort, HeterogeneousNodeApp::SUBNET_LTE, 200, Seconds (0.5),
                  false, Ipv4Address ("0.0.0.0"), "LTE UE application started.");
    }
  // 在仿真时刻输出（依赖 Simulator::Now），与 Adhoc/DataLink 的 SPN_ANNOUNCE 格式一致，供可视化动态回程
  if (m_spnLte)
    {
      const double lteAnnT = std::max (0.0, GetNodeJoinTime (m_spnLte->GetId ()));
      Simulator::Schedule (Seconds (lteAnnT), &HeterogeneousNmsFramework::EmitLteSpnAnnounce, this);
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

      // 业务流采用持续发送模型：不再依赖 scenario 的 stopTime，避免中途归零。
      std::string bizRate = "500Kbps";
      if (fcfg.type == "control")
        {
          bizRate = "100Kbps";
        }
      else if (fcfg.type == "video")
        {
          bizRate = "1.5Mbps";
        }
      OnOffHelper onOff ("ns3::UdpSocketFactory", Address ());
      onOff.SetAttribute ("Remote", AddressValue (InetSocketAddress (dstAddr, port)));
      onOff.SetAttribute ("DataRate", DataRateValue (DataRate (bizRate.c_str ())));
      onOff.SetAttribute ("PacketSize", UintegerValue (1024));
      // Force CBR send pattern to avoid OnOff duty-cycle throughput loss.
      onOff.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1000]"));
      onOff.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
      ApplicationContainer clientApps = onOff.Install (srcNode);
      double stagger = static_cast<double> (fcfg.flowId % 5) * 0.05; // avoid synchronized burst starts
      clientApps.Start (Seconds (fcfg.startTime + stagger));
      clientApps.Stop (Seconds (m_simTimeSeconds));
      {
        const uint8_t ipTos = MapBusinessPriorityToIpTos (fcfg.priority);
        Ptr<OnOffApplication> onOffApp = DynamicCast<OnOffApplication> (clientApps.Get (0));
        const double tosAt = fcfg.startTime + stagger + 0.0001;
        Simulator::Schedule (Seconds (tosAt), [onOffApp, ipTos, fcfg] () {
            if (!onOffApp)
              {
                return;
              }
            Ptr<Socket> sk = onOffApp->GetSocket ();
            if (sk)
              {
                sk->SetIpTos (ipTos);
              }
            std::ostringstream qoss;
            qoss << "flowId=" << fcfg.flowId << " ipTos=0x" << std::hex << static_cast<unsigned> (ipTos)
                 << std::dec << " priority=" << static_cast<uint32_t> (fcfg.priority);
            NmsLog ("INFO", fcfg.srcNodeId, "FLOW_QOS", qoss.str ());
          });
      }
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

      Simulator::Schedule (Seconds (fcfg.startTime), [this, fcfg, getConfiguredPath, bizRate] () {
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
              << " size=" << 1024 << " rate=" << bizRate
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
        if (it->second.initialEnergy >= 0.0) energy = it->second.initialEnergy;
        if (it->second.initialLinkQuality >= 0.0) linkQ = it->second.initialLinkQuality;
        if (now < it->second.joinTime)
          {
            joinState = "not_joined";
          }
      }
    Vector pos (0, 0, 0);
    Ptr<MobilityModel> mob = node->GetObject<MobilityModel> ();
    if (mob) pos = mob->GetPosition ();
    std::string ip = GetNodePrimaryIpv4 (node);
    if (ip.empty () && it != m_joinConfig.end () && !it->second.ipAddress.empty ())
      {
        ip = it->second.ipAddress;
      }
    Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
    bool anyIfaceUp = false;
    if (ipv4)
      {
        for (uint32_t i = 1; i < ipv4->GetNInterfaces (); ++i)
          {
            if (ipv4->IsUp (i))
              {
                anyIfaceUp = true;
                break;
              }
          }
      }
    if (ipv4 && ipv4->GetNInterfaces () >= 2 && !anyIfaceUp)
      {
        // 未到 join_time 的接口关闭（NODE_DOWN）记为 pending_join，避免与退网 offline 混淆
        if (it != m_joinConfig.end () && now < it->second.joinTime)
          {
            joinState = "pending_join";
          }
        else
          {
            // 已到 join_time 但接口尚未 Up：若应用仍在运行，视为入网过程中（BringUp 与 JSONL 采样时序），勿标 offline
            bool appRunning = false;
            for (uint32_t ai = 0; ai < node->GetNApplications (); ++ai)
              {
                Ptr<HeterogeneousNodeApp> ap = DynamicCast<HeterogeneousNodeApp> (node->GetApplication (ai));
                if (ap && ap->IsApplicationRunning ())
                  {
                    appRunning = true;
                    break;
                  }
              }
            joinState = appRunning ? "pending_join" : "offline";
          }
      }

    std::string role;
    std::ostringstream chExtra;
    for (uint32_t a = 0; a < node->GetNApplications (); ++a)
      {
        Ptr<HeterogeneousNodeApp> app = DynamicCast<HeterogeneousNodeApp> (node->GetApplication (a));
        if (!app) continue;
        if (app->IsApplicationRunning ())
          {
            energy = app->GetUvMibEnergy ();
            linkQ = app->GetUvMibLinkQuality ();
          }
        switch (app->GetSubnetType ())
          {
          case HeterogeneousNodeApp::SUBNET_ADHOC:
            if (app->IsApplicationRunning ())
              {
                chExtra << ",\"cluster_role\":\"" << (app->IsClusterHead () ? "CH" : "CM") << "\"";
                auto cs = app->GetAdhocChScoreSnapshot ();
                chExtra << std::fixed << std::setprecision (4)
                        << ",\"ch_score\":{\"total\":" << cs.total << ",\"energy\":" << cs.energy
                        << ",\"degree\":" << cs.degree << ",\"mobility\":" << cs.mobility
                        << ",\"centrality\":" << cs.centrality << "}";
              }
            if (app->IsSpn ())
              {
                role = "PRIMARY_SPN";
              }
            else if (app->IsBackupSpn ())
              {
                role = "BACKUP_SPN";
              }
            else
              {
                role = "TSN";
              }
            break;
          case HeterogeneousNodeApp::SUBNET_DATALINK:
            if (app->IsSpn ())
              {
                role = "PRIMARY_SPN";
              }
            else if (app->IsBackupSpn ())
              {
                role = "BACKUP_SPN";
              }
            else
              {
                role = "TSN";
              }
            break;
          case HeterogeneousNodeApp::SUBNET_LTE:
            if (app->IsSpn ())
              {
                role = "PRIMARY_SPN";
              }
            else
              {
                role = "TSN";
              }
            break;
          default:
            break;
          }
        break;
      }
    if (role.empty () && it != m_joinConfig.end ())
      {
        std::string tp = it->second.type;
        for (char& c : tp) c = static_cast<char> (::tolower (static_cast<unsigned char> (c)));
        if (tp == "gmc")
          {
            role = "GMC";
          }
        else if (tp == "enb")
          {
            role = "PRIMARY_SPN";
          }
        else if (tp == "ue")
          {
            role = "UE";
          }
        else
          {
            std::string sub = it->second.subnet;
            for (char& c : sub) c = static_cast<char> (::tolower (static_cast<unsigned char> (c)));
            if (sub == "adhoc" || sub == "datalink")
              {
                role = "TSN";
              }
          }
      }
    std::string offlineExtra;
    auto itOr = m_nodeOfflineSystemReason.find (nodeId);
    if (itOr != m_nodeOfflineSystemReason.end ())
      {
        offlineExtra = ",\"offline_reason\":\"" + itOr->second + "\"";
      }
    WriteJsonlStateLine (nodeId, joinState, pos.x, pos.y, pos.z, ip, energy, linkQ, role,
                         chExtra.str () + offlineExtra);
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
  double sumLossPct = 0.0;
  double sumDelayMs = 0.0;
  uint32_t metricFlows = 0;
  uint32_t zeroThrFlows = 0;
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
      sumLossPct += lossPct;
      sumDelayMs += delayMs;
      metricFlows++;
      if (thrMbps <= 1e-6)
        {
          zeroThrFlows++;
        }
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
  if (m_routeAdaptRuntimeEnable && m_routingModeEffective == ROUTING_OLSR &&
      metricFlows > 0 &&
      (m_routeAdaptRuntimeLastEvalTs < 0.0 ||
       now - m_routeAdaptRuntimeLastEvalTs >= m_routeAdaptRuntimeWindowSec))
    {
      m_routeAdaptRuntimeLastEvalTs = now;
      double avgLossPct = sumLossPct / static_cast<double> (metricFlows);
      double avgDelayMs = sumDelayMs / static_cast<double> (metricFlows);
      NmsLog ("INFO", 0, "ROUTE_ADAPT_RUNTIME_METRIC",
              "avgLossPct=" + std::to_string (avgLossPct) +
              " avgDelayMs=" + std::to_string (avgDelayMs) +
              " zeroThrFlows=" + std::to_string (zeroThrFlows));
      MaybeUpdateRuntimeRouteAdapt (avgLossPct, avgDelayMs, zeroThrFlows, now);
    }
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
  HeterogeneousNodeApp::ResetSharedElectionState ();

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
  ScenarioConfig loadedScenario;
  bool hasScenario = false;
  if (!m_scenarioConfigPath.empty ())
    {
      loadedScenario = LoadScenarioConfig (m_scenarioConfigPath);
      hasScenario = true;
      if (loadedScenario.spnElectionTimeoutSec > 0.0)
        {
          m_spnElectionTimeoutSec = loadedScenario.spnElectionTimeoutSec;
        }
      if (loadedScenario.spnHeartbeatMissThreshold > 0)
        {
          m_spnHeartbeatMissThreshold = loadedScenario.spnHeartbeatMissThreshold;
        }
      NmsLog ("INFO", 0, "SCENARIO", "Loaded scenario: id=" + (loadedScenario.scenarioId.empty () ? "none" : loadedScenario.scenarioId)
              + " from " + m_scenarioConfigPath);
      for (const auto& ev : loadedScenario.events)
        {
          if (ev.type == "SPN_SWITCH" || ev.type == "SPN_FORCE_SWITCH")
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
      m_energyDeltaThreshold = 0.15;
      m_stateSuppressWindowSec = 15.0;
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
  if (m_routeAdaptConfigured == ADAPT_AUTO)
    {
      m_routeAdaptEffective = DecideRouteAdaptLevelAuto (hasScenario ? &loadedScenario : nullptr);
    }
  else
    {
      m_routeAdaptEffective = m_routeAdaptConfigured;
    }
  ApplyAdhocRoutingAdaptiveParams ();
  if (m_routingModeConfigured == ROUTING_AUTO)
    {
      m_routingModeEffective = DecideRoutingModeAuto (hasScenario ? &loadedScenario : nullptr);
    }
  else
    {
      m_routingModeEffective = m_routingModeConfigured;
    }
  NmsLog ("INFO", 0, "ROUTING_POLICY",
          "configured=" + RoutingModeToString (m_routingModeConfigured) +
          " effective=" + RoutingModeToString (m_routingModeEffective) +
          " decision_scope=pre_run_gmc");
  NmsLog ("INFO", 0, "ROUTE_ADAPT_POLICY",
          "configured=" + RouteAdaptLevelToString (m_routeAdaptConfigured) +
          " effective=" + RouteAdaptLevelToString (m_routeAdaptEffective) +
          " olsr_hello=" + std::to_string (m_adhocOlsrHelloSec) +
          " olsr_tc=" + std::to_string (m_adhocOlsrTcSec) +
          " aodv_hello=" + std::to_string (m_adhocAodvHelloSec) +
          " aodv_active_to=" + std::to_string (m_adhocAodvActiveRouteTimeoutSec) +
          " aodv_loss=" + std::to_string (m_adhocAodvAllowedHelloLoss));
  m_routeAdaptRuntimeLastSwitchTs = -1.0;
  m_routeAdaptRuntimeLastEvalTs = -1.0;
  m_routeAdaptUpgradeVotes = 0;
  m_routeAdaptDowngradeVotes = 0;
  NmsLog ("INFO", 0, "ROUTE_ADAPT_RUNTIME",
          std::string ("enabled=") + (m_routeAdaptRuntimeEnable ? "1" : "0") +
          " windowSec=" + std::to_string (m_routeAdaptRuntimeWindowSec) +
          " cooldownSec=" + std::to_string (m_routeAdaptRuntimeCooldownSec));
  NmsLog ("INFO", 0, "DATALINK_ROUTING",
          "mode=managed_datalink static_only dynamic_l3_disabled");
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

  // === 3.9 业务流区分服务：在无线网卡上安装 PfifoFast（依赖 SocketPriorityTag；见 InstallApplications 中 SetIpTos） ===
  InstallBusinessFlowQosQueueDiscs ();

  // === 4. 安装自定义应用 ===
  InstallApplications ();

  // === 4.2 场景事件：NODE_OFFLINE/NODE_FAIL 直接退网；NODE_ENERGY_FAIL/LINK_INTERFERENCE_FAIL 先注入再周期监测 ===
  if (!m_scenarioConfigPath.empty ())
    {
      ScenarioConfig scEv = LoadScenarioConfig (m_scenarioConfigPath);
      uint32_t gmcIdSched = m_gmcNode.Get (0)->GetId ();
      for (const auto& ev : scEv.events)
        {
          if (ev.type == "NODE_ENERGY_FAIL")
            {
              if (ev.target == 0 || ev.target == gmcIdSched)
                {
                  NmsLog ("WARN", 0, "SYSTEM", "NODE_ENERGY_FAIL ignored: invalid target");
                  continue;
                }
              if (ev.injectedEnergy < 0.0 || ev.threshold < 0.0)
                {
                  NmsLog ("WARN", 0, "SYSTEM",
                          "NODE_ENERGY_FAIL ignored: need injected_energy and threshold (node "
                          + std::to_string (ev.target) + ")");
                  continue;
                }
              double tInj = (ev.injectTime >= 0.0) ? ev.injectTime : ev.time;
              const uint32_t tgt = ev.target;
              const double injE = ev.injectedEnergy;
              Simulator::Schedule (Seconds (tInj), [this, tgt, injE] () { ScenarioInjectEnergy (tgt, injE); });
              PhysicsWatchEntry w;
              w.target = ev.target;
              w.kind = PhysicsWatchKind::ENERGY;
              w.threshold = ev.threshold;
              w.done = false;
              m_physicsWatches.push_back (w);
              NmsLog ("INFO", 0, "SYSTEM",
                      "Scheduled NODE_ENERGY_FAIL inject at t=" + std::to_string (tInj) + "s target=Node "
                      + std::to_string (ev.target) + " threshold=" + std::to_string (ev.threshold));
              continue;
            }
          if (ev.type == "LINK_INTERFERENCE_FAIL")
            {
              if (ev.target == 0 || ev.target == gmcIdSched)
                {
                  NmsLog ("WARN", 0, "SYSTEM", "LINK_INTERFERENCE_FAIL ignored: invalid target");
                  continue;
                }
              if (ev.injectedLinkQuality < 0.0 || ev.threshold < 0.0)
                {
                  NmsLog ("WARN", 0, "SYSTEM",
                          "LINK_INTERFERENCE_FAIL ignored: need injected_link_quality and threshold (node "
                          + std::to_string (ev.target) + ")");
                  continue;
                }
              double tInj = (ev.injectTime >= 0.0) ? ev.injectTime : ev.time;
              const uint32_t tgt = ev.target;
              const double injQ = ev.injectedLinkQuality;
              Simulator::Schedule (Seconds (tInj), [this, tgt, injQ] () { ScenarioInjectLinkQuality (tgt, injQ); });
              PhysicsWatchEntry w;
              w.target = ev.target;
              w.kind = PhysicsWatchKind::LINK;
              w.threshold = ev.threshold;
              w.done = false;
              m_physicsWatches.push_back (w);
              NmsLog ("INFO", 0, "SYSTEM",
                      "Scheduled LINK_INTERFERENCE_FAIL inject at t=" + std::to_string (tInj) + "s target=Node "
                      + std::to_string (ev.target) + " threshold=" + std::to_string (ev.threshold));
              continue;
            }
          bool isOff = (ev.type == "NODE_OFFLINE" || ev.type == "NODE_FAIL" || ev.type == "NODE_LEAVE");
          if (!isOff || ev.target == 0)
            continue;
          if (ev.target == gmcIdSched)
            {
              NmsLog ("WARN", 0, "SYSTEM",
                      "Scenario event ignored: offline target cannot be GMC (node " + std::to_string (gmcIdSched) + ")");
              continue;
            }
          std::string rk = ev.offlineReason;
          if (rk.empty ())
            {
              rk = (ev.type == "NODE_LEAVE") ? "voluntary" : "fault";
            }
          const double tOff = ev.time;
          Simulator::Schedule (Seconds (tOff), [this, ev, rk] () {
            InjectNodeOffline (ev.target, rk, std::string ("DIRECT_FAIL"), -1.0, -1.0, -1.0);
          });
          NmsLog ("INFO", 0, "SYSTEM",
                  "Scheduled NODE_OFFLINE at t=" + std::to_string (tOff) + "s target=Node " + std::to_string (ev.target)
                  + " reason=" + rk);
        }
      if (!m_physicsWatches.empty ())
        {
          Simulator::Schedule (Seconds (1.0), &HeterogeneousNmsFramework::OnPhysicsMonitorTick, this);
        }
    }

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
      uint32_t gmcIdFail = m_gmcNode.Get (0)->GetId ();
      if (m_failNodeId == gmcIdFail)
        {
          NmsLog ("WARN", 0, "SYSTEM", "Event injection ignored: cannot target GMC node " + std::to_string (gmcIdFail));
        }
      else
        {
          const uint32_t fnid = m_failNodeId;
          const double ft = m_failTime;
          Simulator::Schedule (Seconds (ft), [this, fnid] () {
            InjectNodeOffline (fnid, std::string ("fault"), std::string ("DIRECT_FAIL"), -1.0, -1.0, -1.0);
          });
          NmsLog ("INFO", 0, "SYSTEM",
                  "Event injection: NODE_OFFLINE (fault) Node " + std::to_string (m_failNodeId) + " at t="
                  + std::to_string (m_failTime) + "s");
        }
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

  // 与 README 标准流程对齐：仿真完成后自动导出可视化数据到 visualization/data
  if (!m_outputDir.empty ())
    {
      std::ostringstream exportCmd;
      exportCmd << "python3 export_visualization_data.py \"" << m_outputDir
                << "\" -o visualization/data 2>/dev/null";
      int exportRet = std::system (exportCmd.str ().c_str ());
      if (exportRet == 0)
        {
          NmsLog ("INFO", 0, "VIS_EXPORT", "visualization data exported to visualization/data from " + m_outputDir);
        }
      else
        {
          NmsLog ("WARN", 0, "VIS_EXPORT", "export_visualization_data.py failed for " + m_outputDir);
        }
    }

  if (g_nmsLogFile.is_open ())
    {
      g_nmsLogFile << "========== Simulation finished ==========" << std::endl;
      g_nmsLogFile.close ();
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
  double simTime = 90.0;
  int adhocTopology = 0;  // 0=MESH, 1=STAR, 2=TREE
  std::string adhocTopologyStr = "mesh";  // 可选：--adhoc-topology=star|tree|mesh
  bool enablePacketParse = true;
  bool enablePcap = false;
  bool useDualChannel = false;
  std::string joinConfigPath;
  std::string scenarioConfigPath;
  uint32_t failNodeId = 0;   // 事件注入：要断电的节点 ID，0=禁用
  double failTime = 30.0;    // 断电时刻（秒）
  double energyDeltaThreshold = 0.15;
  double stateSuppressWindow = 15.0;
  double aggregateInterval = 2.0;
  std::string scenarioMode = "normal";
  std::string routeMode = "auto";
  std::string routeAdapt = "auto";
  bool routeAdaptRuntime = false;
  double routeAdaptRuntimeWindow = 5.0;
  uint32_t rngSeed = 1;
  uint32_t rngRun = 1;
  std::string hnmsConfigPath;
  int jsonLogFlag = 0;

  CommandLine cmd;
  cmd.AddValue ("simTime", "Simulation time in seconds", simTime);
  cmd.AddValue ("topology", "Adhoc topology: 0=MESH, 1=STAR, 2=TREE", adhocTopology);
  cmd.AddValue ("adhoc-topology", "Adhoc topology: mesh|star|tree (overrides topology if set)", adhocTopologyStr);
  cmd.AddValue ("parsePackets",
                "Runtime TLV/HNMP business trace (TLV_TRACE/HELLO_TRACE/POLICY_TRACE); default on, use 0 to disable",
                enablePacketParse);
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
  cmd.AddValue ("routeMode", "Routing mode for Adhoc/DataLink: auto|aodv|olsr", routeMode);
  cmd.AddValue ("routeAdapt", "Adhoc routing adaptive level: auto|stable|degraded|critical", routeAdapt);
  cmd.AddValue ("routeAdaptRuntime", "Enable runtime OLSR parameter adaptation (0/1), default 0", routeAdaptRuntime);
  cmd.AddValue ("routeAdaptRuntimeWindow", "Runtime adaptation evaluation window in seconds", routeAdaptRuntimeWindow);
  cmd.AddValue ("rngSeed", "Global RNG seed for reproducible experiments", rngSeed);
  cmd.AddValue ("rngRun", "RNG run index for repeated trials", rngRun);
  cmd.AddValue ("config", "Path to hnms config.json (spn/node/link/qos/simulation); empty=search defaults",
                  hnmsConfigPath);
  cmd.AddValue ("jsonLog", "1=emit structured JSON log lines via StructuredLog", jsonLogFlag);
  cmd.Parse (argc, argv);

#ifdef HNMS_USE_MODULES
  if (hnmsConfigPath.empty ())
    {
      std::ifstream c0 ("config.json");
      if (c0)
        {
          hnmsConfigPath = "config.json";
          c0.close ();
        }
      else
        {
          std::ifstream c1 ("docs/heterogeneous-nms/config.json");
          if (c1)
            {
              hnmsConfigPath = "docs/heterogeneous-nms/config.json";
              c1.close ();
            }
        }
    }
  hnms::ConfigLoader::LoadFromFile (hnmsConfigPath);
  if (std::fabs (simTime - 90.0) < 1e-9)
    {
      simTime = hnms::ConfigLoader::SimulationTotalTime ();
    }
  if (joinConfigPath.empty ())
    {
      joinConfigPath = hnms::ConfigLoader::SimulationJoinConfig ();
    }
  if (scenarioConfigPath.empty ())
    {
      scenarioConfigPath = hnms::ConfigLoader::SimulationScenarioConfig ();
    }
  if (scenarioMode == "normal")
    {
      scenarioMode = hnms::ConfigLoader::SimulationScenarioMode ();
    }
  if (enablePacketParse)
    {
      enablePacketParse = (hnms::ConfigLoader::SimulationParsePackets () != 0u);
    }
  if (!enablePcap)
    {
      enablePcap = (hnms::ConfigLoader::SimulationPcap () != 0u);
    }
  if (!useDualChannel)
    {
      useDualChannel = (hnms::ConfigLoader::SimulationDualChannel () != 0u);
    }
  if (std::fabs (energyDeltaThreshold - 0.15) < 1e-9)
    {
      energyDeltaThreshold = hnms::ConfigLoader::SimulationEnergyDeltaTh ();
    }
  if (std::fabs (stateSuppressWindow - 15.0) < 1e-9)
    {
      stateSuppressWindow = hnms::ConfigLoader::SimulationStateSuppressWin ();
    }
  if (std::fabs (aggregateInterval - 2.0) < 1e-9)
    {
      aggregateInterval = hnms::ConfigLoader::SimulationAggregateInterval ();
    }
  // 仅当未通过 CLI 显式设置时，采用 config.json 的路由/运行时调参参数，避免破坏现有命令行用法。
  if (routeMode == "auto")
    {
      routeMode = hnms::ConfigLoader::SimulationRouteMode ();
    }
  if (routeAdapt == "auto")
    {
      routeAdapt = hnms::ConfigLoader::SimulationRouteAdapt ();
    }
  if (!routeAdaptRuntime)
    {
      routeAdaptRuntime = (hnms::ConfigLoader::SimulationRouteAdaptRuntime () != 0u);
    }
  if (std::fabs (routeAdaptRuntimeWindow - 5.0) < 1e-9)
    {
      routeAdaptRuntimeWindow = hnms::ConfigLoader::SimulationRouteAdaptRuntimeWindow ();
    }
  hnms::StructuredLog::SetJsonEnabled (jsonLogFlag != 0);
  hnms::EventScheduler::Instance ().SetGlobalMaxRetry (5);
  {
    std::ostringstream o;
    o << "config=" << (hnmsConfigPath.empty () ? "(defaults)" : hnmsConfigPath)
      << " jsonLog=" << jsonLogFlag;
    NmsLog ("INFO", 0, "HNMS_ARCH", o.str ());
  }
#endif

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
  framework.SetRoutingMode (routeMode);
  framework.SetRouteAdaptLevel (routeAdapt);
  framework.SetRouteAdaptRuntime (routeAdaptRuntime);
  framework.SetRouteAdaptRuntimeWindowSec (routeAdaptRuntimeWindow);
  framework.SetRouteAdaptRuntimeCooldownSec (
#ifdef HNMS_USE_MODULES
      hnms::ConfigLoader::SimulationRouteAdaptRuntimeCooldown ()
#else
      20.0
#endif
  );
  if (!joinConfigPath.empty ()) framework.SetJoinConfigPath (joinConfigPath);
  if (!scenarioConfigPath.empty ()) framework.SetScenarioConfigPath (scenarioConfigPath);
  if (failNodeId > 0) { framework.SetFailNodeId (failNodeId); framework.SetFailTime (failTime); }
#ifdef HNMS_USE_MODULES
  // 命令行已覆盖的参数（能量阈值/抑制窗口/聚合周期等）不再读 config；此处仅注入无 CLI 对应项的 SPN 参数（默认与构造函数一致）
  {
    uint32_t hb = hnms::ConfigLoader::SpnHeartbeatMissThreshold ();
    uint8_t hbu = static_cast<uint8_t> (std::min (255u, std::max (1u, hb)));
    framework.SetFrameworkSpnElectionTunables (hnms::ConfigLoader::SpnElectionWaitTime (), hbu);
  }
#endif
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

