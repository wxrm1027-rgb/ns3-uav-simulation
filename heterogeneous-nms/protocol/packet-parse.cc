#include "packet-parse.h"
#include "../core/logger.h"
#include "tlv.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace NmsPacketParse
{
static bool g_enablePacketParse = false;

void SetEnable (bool enable) { g_enablePacketParse = enable; }
bool IsEnabled () { return g_enablePacketParse; }

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

void LogHelloPacket (uint32_t nodeId, const char* direction,
                     ns3::Ipv4Address fromAddr, uint16_t fromPort,
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

void LogPolicyPacket (uint32_t nodeId, const char* direction,
                      ns3::Ipv4Address fromAddr, uint16_t fromPort,
                      const uint8_t* data, uint32_t len)
{
  if (!IsEnabled ()) return;
  std::ostringstream oss;
  oss << "[" << direction << "] NodeId=" << nodeId << " From=" << fromAddr << ":" << fromPort
      << " Size=" << len << "B " << ToHex (data, std::min (len, 32u));
  if (len >= 3) oss << " " << ParseTlvValue (data, len);
  NmsLog ("INFO", nodeId, "PKT_PARSE_POLICY", oss.str ());
}
} // namespace NmsPacketParse

