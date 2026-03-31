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
      if (type == NmsTlv::TYPE_TELEMETRY_010 && valLen >= 24)
        {
          double e, q, m;
          std::memcpy (&e, data + 3, 8);
          std::memcpy (&q, data + 11, 8);
          std::memcpy (&m, data + 19, 8);
          oss << " [energy=" << std::fixed << std::setprecision (3) << e
              << ", linkQ=" << q << ", mobility=" << m << "]";
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

