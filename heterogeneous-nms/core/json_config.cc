#include "json_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace {

bool
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

} // namespace

std::map<uint32_t, NodeJoinConfig>
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
        while (start < content.size () &&
               (content[start] == ' ' || content[start] == '\t' ||
                content[start] == '\n' || content[start] == '\r'))
          ++start;
        if (start >= content.size ()) return 0.0;
        size_t end = start;
        while (end < content.size () &&
               (std::isdigit (content[end]) || content[end] == '.' ||
                content[end] == '-' || content[end] == '+' ||
                content[end] == 'e' || content[end] == 'E'))
          ++end;
        std::string s = content.substr (start, end - start);
        return std::atof (s.c_str ());
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
      size_t pNeigh = content.find ("\"neighbors\"", i);
      if (pNeigh != std::string::npos && pNeigh < content.find ("\"node_id\"", i + 10))
        {
          size_t bracket = content.find ('[', pNeigh);
          if (bracket != std::string::npos)
            {
              size_t end = bracket + 1;
              while (end < content.size ())
                {
                  while (end < content.size () && (content[end] == ' ' || content[end] == '\t' || content[end] == '\n' || content[end] == '\r' || content[end] == ','))
                    ++end;
                  if (end >= content.size () || content[end] == ']') break;
                  char* tail = nullptr;
                  long v = std::strtol (content.c_str () + end, &tail, 10);
                  if (tail == content.c_str () + end) break;
                  if (v >= 0) c.neighbors.push_back (static_cast<uint32_t> (v));
                  end = static_cast<size_t> (tail - content.c_str ());
                }
            }
        }
      out[c.nodeId] = c;
      size_t objEnd = content.find ('}', i);
      if (objEnd == std::string::npos) break;
      i = objEnd + 1;
    }
  return out;
}

std::string
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

ScenarioConfig
LoadScenarioConfig (const std::string& path)
{
  ScenarioConfig out;
  out.spnElectionTimeoutSec = 2.0;
  out.spnHeartbeatMissThreshold = 3;
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

  auto parseTopNum = [&content] (const std::string& key, double defv) -> double {
    size_t p = content.find ("\"" + key + "\"");
    if (p == std::string::npos) return defv;
    size_t c = content.find (':', p);
    if (c == std::string::npos) return defv;
    return std::atof (content.c_str () + c + 1);
  };
  double electionTimeout = parseTopNum ("spn_election_timeout", out.spnElectionTimeoutSec);
  double hbMissThreshold = parseTopNum ("spn_heartbeat_miss_threshold", static_cast<double> (out.spnHeartbeatMissThreshold));
  if (electionTimeout > 0.0) out.spnElectionTimeoutSec = electionTimeout;
  if (hbMissThreshold >= 1.0) out.spnHeartbeatMissThreshold = static_cast<uint32_t> (hbMissThreshold);
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
              ev.injectTime = -1.0;
              ev.target = 0;
              ev.triggerNodeId = 0;
              ev.newSpnNodeId = 0;
              ev.offlineReason = "";
              ev.injectedEnergy = -1.0;
              ev.injectedLinkQuality = -1.0;
              ev.threshold = -1.0;
              size_t tPos = obj.find ("\"time\"");
              if (tPos != std::string::npos)
                {
                  size_t colon = obj.find (':', tPos);
                  if (colon != std::string::npos) ev.time = std::atof (obj.c_str () + colon + 1);
                }
              size_t typePos = obj.find ("\"type\"");
              if (typePos != std::string::npos)
                {
                  size_t q1 = obj.find ('"', obj.find (':', typePos) + 1);
                  size_t q2 = obj.find ('"', q1 + 1);
                  if (q1 != std::string::npos && q2 != std::string::npos) ev.type = obj.substr (q1 + 1, q2 - q1 - 1);
                }
              size_t targetPos = obj.find ("\"target\"");
              if (targetPos != std::string::npos)
                {
                  size_t colon = obj.find (':', targetPos);
                  if (colon != std::string::npos) ev.target = static_cast<uint32_t> (std::atoi (obj.c_str () + colon + 1));
                }
              size_t triggerPos = obj.find ("\"trigger_node\"");
              if (triggerPos != std::string::npos)
                {
                  size_t colon = obj.find (':', triggerPos);
                  if (colon != std::string::npos) ev.triggerNodeId = static_cast<uint32_t> (std::atoi (obj.c_str () + colon + 1));
                }
              size_t newSpnPos = obj.find ("\"new_spn_node\"");
              if (newSpnPos == std::string::npos)
                newSpnPos = obj.find ("\"new_spn\"");
              if (newSpnPos != std::string::npos)
                {
                  size_t colon = obj.find (':', newSpnPos);
                  if (colon != std::string::npos) ev.newSpnNodeId = static_cast<uint32_t> (std::atoi (obj.c_str () + colon + 1));
                }
              // 兼容字段：trigger_node 未给时回退 target
              if (ev.triggerNodeId == 0)
                ev.triggerNodeId = ev.target;
              size_t reasonPos = obj.find ("\"reason\"");
              if (reasonPos != std::string::npos)
                {
                  size_t q1 = obj.find ('"', obj.find (':', reasonPos) + 1);
                  size_t q2 = obj.find ('"', q1 + 1);
                  if (q1 != std::string::npos && q2 != std::string::npos)
                    ev.offlineReason = obj.substr (q1 + 1, q2 - q1 - 1);
                }
              {
                double tmpD = 0.0;
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
              if (!ev.type.empty ()) out.events.push_back (ev);
              pos = objEnd + 1;
            }
        }
    }
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
                return std::atof (obj.c_str () + c + 1);
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
              if (bf.flowId > 0 && bf.srcNodeId != bf.dstNodeId) out.businessFlows.push_back (bf);
              pos = objEnd + 1;
            }
        }
    }
  return out;
}

