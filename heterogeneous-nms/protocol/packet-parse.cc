#include "packet-parse.h"
#include "../core/logger.h"
#include "tlv.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace NmsPacketParse
{
static bool g_enablePacketParse = true;

void SetEnable (bool enable) { g_enablePacketParse = enable; }
bool IsEnabled () { return g_enablePacketParse; }

/// 单行业务日志：HNMP 头 + 多段 TLV 摘要（兼容旧版 3 字节 HNMP 头）
static std::string
FormatHnmpTlvTraceLine (const char* direction, uint32_t nodeId,
                        ns3::Ipv4Address srcAddr, ns3::Ipv4Address dstAddr, uint16_t srcPort,
                        uint16_t dstPort, const uint8_t* data, uint32_t len)
{
  std::ostringstream biz;
  biz << "dir=" << direction << " node=" << nodeId << " src=" << srcAddr << ":" << srcPort
      << " dst=" << dstAddr << ":" << dstPort << " bytes=" << len;
  uint32_t off = 0;
  if (len >= 6 && len == 6u + data[5])
    {
      off = 6;
      biz << " hnmp=ft=0x" << std::hex << static_cast<int> (data[0]) << " qos=" << static_cast<int> (data[1])
          << " srcId=" << static_cast<int> (data[2]) << " dstId=" << static_cast<int> (data[3])
          << " seq=" << static_cast<int> (data[4]) << " payLen=" << static_cast<int> (data[5]) << std::dec;
    }
  else if (len >= 3 && len == 3u + data[2])
    {
      off = 3;
      biz << " hnmp=legacy3B ft=0x" << std::hex << static_cast<int> (data[0]) << " qos="
          << static_cast<int> (data[1]) << " payLen=" << static_cast<int> (data[2]) << std::dec;
    }
  else if (len >= 6)
    {
      off = 6;
      biz << " hnmp=ft=0x" << std::hex << static_cast<int> (data[0]) << " qos=" << static_cast<int> (data[1])
          << " srcId=" << static_cast<int> (data[2]) << " dstId=" << static_cast<int> (data[3])
          << " seq=" << static_cast<int> (data[4]) << " payLen=" << static_cast<int> (data[5])
          << std::dec << " (parse-unverified)";
    }
  else if (len >= 3)
    {
      biz << " payload=rawTlv";
      off = 0;
    }
  else
    {
      return biz.str () + " payload=tooShort";
    }
  bool first = true;
  while (off + 3 <= len)
    {
      uint16_t vlen = (static_cast<uint16_t> (data[off + 1]) << 8) | data[off + 2];
      if (off + 3u + vlen > len)
        {
          break;
        }
      if (first)
        {
          biz << " | ";
          first = false;
        }
      else
        {
          biz << " ; ";
        }
      biz << ParseTlvValue (data + off, 3u + vlen);
      off += 3u + vlen;
    }
  return biz.str ();
}

std::string ToHex (const uint8_t* data, uint32_t len, uint32_t maxBytes)
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

std::string ParseTlvValue (const uint8_t* data, uint32_t len)
{
  if (len < 3) return "(too short)";
  uint8_t type = data[0];
  uint16_t valLen = (static_cast<uint16_t> (data[1]) << 8) | data[2];
  std::ostringstream oss;
  const char* typeStr = "Unknown";
  if (type == NmsTlv::TYPE_TELEMETRY_010) typeStr = "Telemetry010(0x10)";
  else if (type == NmsTlv::TYPE_ROLE_011) typeStr = "Role(0x11)";
  else if (type == NmsTlv::TYPE_SUBNET_012) typeStr = "ConfigModel(0x12)";
  else if (type == NmsTlv::TYPE_INTENT_013) typeStr = "Intent013(0x13)";
  else if (type == NmsTlv::TYPE_EXEC_CMD_014) typeStr = "ExecCmd014(0x14)";
  else if (type == NmsTlv::TYPE_EXEC_RESULT_015) typeStr = "ExecResult015(0x15)";
  else if (type == NmsTlv::TYPE_INTENT_REPORT_016) typeStr = "IntentReport016(0x16)";
  else if (type == NmsTlv::TYPE_FLOW_020) typeStr = "BusinessModel(0x20)";
  else if (type == NmsTlv::TYPE_TOPO_030) typeStr = "TopologyModel(0x30)";
  else if (type == NmsTlv::TYPE_LINK_031) typeStr = "TopologyModel-LinkCtrl(0x31)";
  else if (type == NmsTlv::TYPE_FAULT_040) typeStr = "Alert040(0x40)";
  else if (type == NmsTlv::TYPE_ROUTE_FAIL_041) typeStr = "RouteFail(0x41)";
  else if (type == NmsTlv::TYPE_HELLO_ELECTION) typeStr = "HelloElection";
  else if (type == NmsTlv::TYPE_NODE_REPORT_SPN) typeStr = "NodeReportSpn";
  else if (type == NmsTlv::TYPE_TOPOLOGY_AGGREGATE) typeStr = "TopologyAggregate";
  else if (type == NmsTlv::TYPE_SCORE_FLOOD) typeStr = "ScoreFlood";
  oss << "Type=" << typeStr << "(0x" << std::hex << (int)type << std::dec << "), Len=" << valLen;
  if (len >= 3u + valLen)
    {
      if (type == NmsTlv::TYPE_TELEMETRY_010 && valLen == NmsTlv::TELEMETRY_010_VALUE_LEN)
        {
          NmsTlv::Telemetry010 t = {};
          if (NmsTlv::ParseTelemetry010 (data, len, &t) > 0)
            {
              oss << " [NodeID=" << static_cast<uint32_t> (t.nodeId)
                  << ", BatteryLevel=" << static_cast<uint32_t> (t.batteryLevel)
                  << ", PosX=" << std::fixed << std::setprecision (3) << t.posX
                  << ", PosY=" << t.posY
                  << ", PosZ=" << t.posZ
                  << ", Vx=" << t.vx
                  << ", Vy=" << t.vy
                  << ", Vz=" << t.vz << "]";
            }
        }
      else if (type == NmsTlv::TYPE_ROLE_011 && valLen == NmsTlv::ROLE_011_VALUE_LEN)
        {
          NmsTlv::Role011 t = {};
          if (NmsTlv::ParseRole011 (data, len, &t) > 0)
            {
              oss << " [NodeID=" << static_cast<uint32_t> (t.nodeId)
                  << ", RoleType=" << static_cast<uint32_t> (t.roleType)
                  << ", ComputeLevel=" << static_cast<uint32_t> (t.computeLevel) << "]";
            }
        }
      else if (type == NmsTlv::TYPE_INTENT_013 && valLen == NmsTlv::INTENT_013_VALUE_LEN)
        {
          NmsTlv::Intent013 t = {};
          if (NmsTlv::ParseIntent013 (data, len, &t) > 0)
            {
              oss << " [IntentID=" << static_cast<uint32_t> (t.intentId)
                  << ", IntentType=" << static_cast<uint32_t> (t.intentType)
                  << ", ValidTime=" << t.validTime
                  << ", Param1=" << t.param1
                  << ", Param2=" << static_cast<uint32_t> (t.param2) << "]";
            }
        }
      else if (type == NmsTlv::TYPE_EXEC_CMD_014 && valLen == NmsTlv::EXEC_CMD_014_VALUE_LEN)
        {
          NmsTlv::ExecCmd014 t = {};
          if (NmsTlv::ParseExecCmd014 (data, len, &t) > 0)
            {
              oss << " [IntentID=" << static_cast<uint32_t> (t.intentId)
                  << ", CmdSeq=" << static_cast<uint32_t> (t.cmdSeq)
                  << ", TargetNodeID=" << static_cast<uint32_t> (t.targetNodeId)
                  << ", CmdType=" << static_cast<uint32_t> (t.cmdType)
                  << ", CmdParam=" << t.cmdParam << "]";
            }
        }
      else if (type == NmsTlv::TYPE_EXEC_RESULT_015 && valLen == NmsTlv::EXEC_RESULT_015_VALUE_LEN)
        {
          NmsTlv::ExecResult015 t = {};
          if (NmsTlv::ParseExecResult015 (data, len, &t) > 0)
            {
              oss << " [IntentID=" << static_cast<uint32_t> (t.intentId)
                  << ", CmdSeq=" << static_cast<uint32_t> (t.cmdSeq)
                  << ", ExecResult=" << static_cast<uint32_t> (t.execResult)
                  << ", FailReason=" << static_cast<uint32_t> (t.failReason) << "]";
            }
        }
      else if (type == NmsTlv::TYPE_INTENT_REPORT_016 && valLen == NmsTlv::INTENT_REPORT_016_VALUE_LEN)
        {
          NmsTlv::IntentReport016 t = {};
          if (NmsTlv::ParseIntentReport016 (data, len, &t) > 0)
            {
              oss << " [IntentID=" << static_cast<uint32_t> (t.intentId)
                  << ", TotalTargets=" << static_cast<uint32_t> (t.totalTargets)
                  << ", SuccessCount=" << static_cast<uint32_t> (t.successCount)
                  << ", FailCount=" << static_cast<uint32_t> (t.failCount) << "]";
            }
        }
      else if (type == NmsTlv::TYPE_FAULT_040 && valLen == NmsTlv::ALERT_040_VALUE_LEN)
        {
          NmsTlv::Alert040 t = {};
          if (NmsTlv::ParseAlert040 (data, len, &t) > 0)
            {
              oss << " [ReportNodeID=" << static_cast<uint32_t> (t.reportNodeId)
                  << ", TargetNodeID=" << static_cast<uint32_t> (t.targetNodeId)
                  << ", AlertLevel=" << static_cast<uint32_t> (t.alertLevel)
                  << ", AlertReason=" << static_cast<uint32_t> (t.alertReason) << "]";
            }
        }
      else if (type == NmsTlv::TYPE_ROUTE_FAIL_041 && valLen == NmsTlv::ROUTE_FAIL_041_VALUE_LEN)
        {
          NmsTlv::RouteFail041 t = {};
          if (NmsTlv::ParseRouteFail041 (data, len, &t) > 0)
            {
              oss << " [FlowID=" << static_cast<uint32_t> (t.flowId)
                  << ", FaultNodeID=" << static_cast<uint32_t> (t.faultNodeId)
                  << ", FailReason=" << static_cast<uint32_t> (t.failReason) << "]";
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
          oss << " [value=\""
              << std::string (reinterpret_cast<const char*> (data + 3), std::min (valLen, (uint16_t)32u))
              << "\"]";
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

void LogDataPacket (uint32_t nodeId, const char* direction,
                    ns3::Ipv4Address srcAddr, ns3::Ipv4Address dstAddr, uint16_t srcPort, uint16_t dstPort,
                    const uint8_t* data, uint32_t len)
{
  if (!IsEnabled () || len == 0) return;
  NmsLog ("INFO", nodeId, "TLV_TRACE", FormatHnmpTlvTraceLine (direction, nodeId, srcAddr, dstAddr, srcPort,
                                                               dstPort, data, len));
}

void LogHelloPacket (uint32_t nodeId, const char* direction,
                     ns3::Ipv4Address fromAddr, uint16_t fromPort,
                     const uint8_t* data, uint32_t len)
{
  if (!IsEnabled () || len == 0) return;
  std::ostringstream biz;
  biz << "dir=" << direction << " node=" << nodeId << " peer=" << fromAddr << ":" << fromPort << " bytes=" << len;
  if (len >= 3)
    {
      biz << " | " << ParseTlvValue (data, len);
    }
  NmsLog ("INFO", nodeId, "HELLO_TRACE", biz.str ());
}

void LogPolicyPacket (uint32_t nodeId, const char* direction,
                      ns3::Ipv4Address fromAddr, uint16_t fromPort,
                      const uint8_t* data, uint32_t len)
{
  if (!IsEnabled () || len == 0) return;
  std::ostringstream biz;
  biz << "dir=" << direction << " node=" << nodeId << " peer=" << fromAddr << ":" << fromPort << " bytes=" << len;
  if (len >= 3)
    {
      biz << " | " << ParseTlvValue (data, len);
    }
  NmsLog ("INFO", nodeId, "POLICY_TRACE", biz.str ());
}
} // namespace NmsPacketParse

