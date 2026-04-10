#include "tlv.h"

#include <algorithm>
#include <cstring>

namespace NmsTlv
{
namespace
{
inline void W16 (uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t> ((v >> 8) & 0xffu); p[1] = static_cast<uint8_t> (v & 0xffu); }
inline uint16_t R16 (const uint8_t* p) { return static_cast<uint16_t> ((static_cast<uint16_t> (p[0]) << 8) | p[1]); }
inline void W32 (uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t> ((v >> 24) & 0xffu); p[1] = static_cast<uint8_t> ((v >> 16) & 0xffu); p[2] = static_cast<uint8_t> ((v >> 8) & 0xffu); p[3] = static_cast<uint8_t> (v & 0xffu); }
inline uint32_t R32 (const uint8_t* p) { return (static_cast<uint32_t> (p[0]) << 24) | (static_cast<uint32_t> (p[1]) << 16) | (static_cast<uint32_t> (p[2]) << 8) | static_cast<uint32_t> (p[3]); }
inline bool Check (const uint8_t* b, uint32_t len, uint8_t t, uint16_t vlen) { return b && len >= 3u + vlen && b[0] == t && R16 (b + 1) == vlen; }
}

void WriteTlvHeader (uint8_t* buf, uint8_t type, uint16_t length)
{
  if (!buf) return;
  buf[0] = type; buf[1] = (length >> 8) & 0xff; buf[2] = length & 0xff;
}

uint32_t BuildTelemetry010Payload (uint8_t* buf, uint32_t sz, const Telemetry010& m)
{
  if (!buf || sz < TELEMETRY_010_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_TELEMETRY_010, TELEMETRY_010_VALUE_LEN);
  uint32_t o = 3; buf[o++] = m.nodeId; buf[o++] = m.batteryLevel;
  std::memcpy (buf + o, &m.posX, 4); o += 4; std::memcpy (buf + o, &m.posY, 4); o += 4;
  std::memcpy (buf + o, &m.posZ, 4); o += 4; std::memcpy (buf + o, &m.vx, 4); o += 4;
  std::memcpy (buf + o, &m.vy, 4); o += 4; std::memcpy (buf + o, &m.vz, 4); o += 4;
  return TELEMETRY_010_TLV_LEN;
}

uint32_t ParseTelemetry010 (const uint8_t* buf, uint32_t len, Telemetry010* out)
{
  if (!out || !Check (buf, len, TYPE_TELEMETRY_010, TELEMETRY_010_VALUE_LEN)) return 0;
  uint32_t o = 3; out->nodeId = buf[o++]; out->batteryLevel = buf[o++];
  std::memcpy (&out->posX, buf + o, 4); o += 4; std::memcpy (&out->posY, buf + o, 4); o += 4;
  std::memcpy (&out->posZ, buf + o, 4); o += 4; std::memcpy (&out->vx, buf + o, 4); o += 4;
  std::memcpy (&out->vy, buf + o, 4); o += 4; std::memcpy (&out->vz, buf + o, 4); o += 4;
  return TELEMETRY_010_TLV_LEN;
}

uint32_t BuildRole011Payload (uint8_t* buf, uint32_t sz, const Role011& m)
{
  if (!buf || sz < ROLE_011_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_ROLE_011, ROLE_011_VALUE_LEN);
  uint32_t o = 3; buf[o++] = m.nodeId; buf[o++] = m.roleType; buf[o++] = m.computeLevel;
  return ROLE_011_TLV_LEN;
}

uint32_t ParseRole011 (const uint8_t* buf, uint32_t len, Role011* out)
{
  if (!out || !Check (buf, len, TYPE_ROLE_011, ROLE_011_VALUE_LEN)) return 0;
  uint32_t o = 3; out->nodeId = buf[o++]; out->roleType = buf[o++]; out->computeLevel = buf[o++];
  return ROLE_011_TLV_LEN;
}

uint32_t BuildSubnet012Payload (uint8_t* buf, uint32_t sz, const Subnet012& m)
{
  if (!buf || sz < SUBNET_012_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_SUBNET_012, SUBNET_012_VALUE_LEN);
  uint32_t o = 3;
  buf[o++] = m.reporterNodeId;
  buf[o++] = m.subnetType;
  buf[o++] = m.qosIntent;
  buf[o++] = m.reserve;
  return SUBNET_012_TLV_LEN;
}

uint32_t ParseSubnet012 (const uint8_t* buf, uint32_t len, Subnet012* out)
{
  if (!out || !Check (buf, len, TYPE_SUBNET_012, SUBNET_012_VALUE_LEN)) return 0;
  uint32_t o = 3;
  out->reporterNodeId = buf[o++];
  out->subnetType = buf[o++];
  out->qosIntent = buf[o++];
  out->reserve = buf[o++];
  return SUBNET_012_TLV_LEN;
}

uint32_t BuildIntent013Payload (uint8_t* buf, uint32_t sz, const Intent013& m)
{
  if (!buf || sz < INTENT_013_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_INTENT_013, INTENT_013_VALUE_LEN);
  uint32_t o = 3; buf[o++] = m.intentId; buf[o++] = m.intentType; W16 (buf + o, m.validTime); o += 2; W16 (buf + o, m.param1); o += 2; buf[o++] = m.param2;
  return INTENT_013_TLV_LEN;
}

uint32_t ParseIntent013 (const uint8_t* buf, uint32_t len, Intent013* out)
{
  if (!out || !Check (buf, len, TYPE_INTENT_013, INTENT_013_VALUE_LEN)) return 0;
  uint32_t o = 3; out->intentId = buf[o++]; out->intentType = buf[o++]; out->validTime = R16 (buf + o); o += 2; out->param1 = R16 (buf + o); o += 2; out->param2 = buf[o++];
  return INTENT_013_TLV_LEN;
}

uint32_t BuildExecCmd014Payload (uint8_t* buf, uint32_t sz, const ExecCmd014& m)
{
  if (!buf || sz < EXEC_CMD_014_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_EXEC_CMD_014, EXEC_CMD_014_VALUE_LEN);
  uint32_t o = 3; buf[o++] = m.intentId; buf[o++] = m.cmdSeq; buf[o++] = m.targetNodeId; buf[o++] = m.cmdType; W32 (buf + o, m.cmdParam); return EXEC_CMD_014_TLV_LEN;
}

uint32_t ParseExecCmd014 (const uint8_t* buf, uint32_t len, ExecCmd014* out)
{
  if (!out || !Check (buf, len, TYPE_EXEC_CMD_014, EXEC_CMD_014_VALUE_LEN)) return 0;
  uint32_t o = 3; out->intentId = buf[o++]; out->cmdSeq = buf[o++]; out->targetNodeId = buf[o++]; out->cmdType = buf[o++]; out->cmdParam = R32 (buf + o);
  return EXEC_CMD_014_TLV_LEN;
}

uint32_t BuildExecResult015Payload (uint8_t* buf, uint32_t sz, const ExecResult015& m)
{
  if (!buf || sz < EXEC_RESULT_015_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_EXEC_RESULT_015, EXEC_RESULT_015_VALUE_LEN);
  uint32_t o = 3; buf[o++] = m.intentId; buf[o++] = m.cmdSeq; buf[o++] = m.execResult; buf[o++] = m.failReason;
  return EXEC_RESULT_015_TLV_LEN;
}

uint32_t ParseExecResult015 (const uint8_t* buf, uint32_t len, ExecResult015* out)
{
  if (!out || !Check (buf, len, TYPE_EXEC_RESULT_015, EXEC_RESULT_015_VALUE_LEN)) return 0;
  uint32_t o = 3; out->intentId = buf[o++]; out->cmdSeq = buf[o++]; out->execResult = buf[o++]; out->failReason = buf[o++];
  return EXEC_RESULT_015_TLV_LEN;
}

uint32_t BuildIntentReport016Payload (uint8_t* buf, uint32_t sz, const IntentReport016& m)
{
  if (!buf || sz < INTENT_REPORT_016_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_INTENT_REPORT_016, INTENT_REPORT_016_VALUE_LEN);
  uint32_t o = 3; buf[o++] = m.intentId; buf[o++] = m.totalTargets; buf[o++] = m.successCount; buf[o++] = m.failCount;
  return INTENT_REPORT_016_TLV_LEN;
}

uint32_t ParseIntentReport016 (const uint8_t* buf, uint32_t len, IntentReport016* out)
{
  if (!out || !Check (buf, len, TYPE_INTENT_REPORT_016, INTENT_REPORT_016_VALUE_LEN)) return 0;
  uint32_t o = 3; out->intentId = buf[o++]; out->totalTargets = buf[o++]; out->successCount = buf[o++]; out->failCount = buf[o++];
  return INTENT_REPORT_016_TLV_LEN;
}

uint32_t BuildFlow020Payload (uint8_t* buf, uint32_t sz, const Flow020& m)
{
  if (!buf || sz < FLOW_020_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_FLOW_020, FLOW_020_VALUE_LEN);
  uint32_t o = 3;
  buf[o++] = m.reporterNodeId;
  W16 (buf + o, m.flowId); o += 2;
  buf[o++] = m.priority;
  std::memcpy (buf + o, &m.throughputMbps, 8); o += 8;
  std::memcpy (buf + o, &m.delayMs, 8); o += 8;
  std::memcpy (buf + o, &m.lossPct, 8); o += 8;
  return FLOW_020_TLV_LEN;
}

uint32_t ParseFlow020 (const uint8_t* buf, uint32_t len, Flow020* out)
{
  if (!out || !Check (buf, len, TYPE_FLOW_020, FLOW_020_VALUE_LEN)) return 0;
  uint32_t o = 3;
  out->reporterNodeId = buf[o++];
  out->flowId = R16 (buf + o); o += 2;
  out->priority = buf[o++];
  std::memcpy (&out->throughputMbps, buf + o, 8); o += 8;
  std::memcpy (&out->delayMs, buf + o, 8); o += 8;
  std::memcpy (&out->lossPct, buf + o, 8); o += 8;
  return FLOW_020_TLV_LEN;
}

uint32_t BuildFlowAgg021Payload (uint8_t* buf, uint32_t sz, const FlowAgg021& m)
{
  if (!buf || sz < FLOW_AGG_021_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_FLOW_AGG_021, FLOW_AGG_021_VALUE_LEN);
  uint32_t o = 3;
  buf[o++] = m.subnetType;
  buf[o++] = m.flowCount;
  buf[o++] = m.highPrioCount;
  buf[o++] = m.mediumPrioCount;
  std::memcpy (buf + o, &m.avgThroughputMbps, 8); o += 8;
  std::memcpy (buf + o, &m.avgDelayMs, 8); o += 8;
  std::memcpy (buf + o, &m.avgLossPct, 8); o += 8;
  return FLOW_AGG_021_TLV_LEN;
}

uint32_t ParseFlowAgg021 (const uint8_t* buf, uint32_t len, FlowAgg021* out)
{
  if (!out || !Check (buf, len, TYPE_FLOW_AGG_021, FLOW_AGG_021_VALUE_LEN)) return 0;
  uint32_t o = 3;
  out->subnetType = buf[o++];
  out->flowCount = buf[o++];
  out->highPrioCount = buf[o++];
  out->mediumPrioCount = buf[o++];
  std::memcpy (&out->avgThroughputMbps, buf + o, 8); o += 8;
  std::memcpy (&out->avgDelayMs, buf + o, 8); o += 8;
  std::memcpy (&out->avgLossPct, buf + o, 8); o += 8;
  return FLOW_AGG_021_TLV_LEN;
}

uint32_t BuildTopo030Payload (uint8_t* buf, uint32_t sz, const Topo030& m)
{
  if (!buf || sz < TOPO_030_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_TOPO_030, TOPO_030_VALUE_LEN);
  uint32_t o = 3;
  buf[o++] = m.reporterNodeId;
  buf[o++] = m.neighborCount;
  std::memcpy (buf + o, &m.avgLinkQuality, 8); o += 8;
  W16 (buf + o, m.routeCost); o += 2;
  W16 (buf + o, m.macRetry); o += 2;
  return TOPO_030_TLV_LEN;
}

uint32_t ParseTopo030 (const uint8_t* buf, uint32_t len, Topo030* out)
{
  if (!out || !Check (buf, len, TYPE_TOPO_030, TOPO_030_VALUE_LEN)) return 0;
  uint32_t o = 3;
  out->reporterNodeId = buf[o++];
  out->neighborCount = buf[o++];
  std::memcpy (&out->avgLinkQuality, buf + o, 8); o += 8;
  out->routeCost = R16 (buf + o); o += 2;
  out->macRetry = R16 (buf + o); o += 2;
  return TOPO_030_TLV_LEN;
}

uint32_t BuildLink031Payload (uint8_t* buf, uint32_t sz, const Link031& m)
{
  if (!buf || sz < LINK_031_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_LINK_031, LINK_031_VALUE_LEN);
  uint32_t o = 3;
  buf[o++] = m.reporterNodeId;
  buf[o++] = m.metricType;
  std::memcpy (buf + o, &m.linkQuality, 8); o += 8;
  return LINK_031_TLV_LEN;
}

uint32_t ParseLink031 (const uint8_t* buf, uint32_t len, Link031* out)
{
  if (!out || !Check (buf, len, TYPE_LINK_031, LINK_031_VALUE_LEN)) return 0;
  uint32_t o = 3;
  out->reporterNodeId = buf[o++];
  out->metricType = buf[o++];
  std::memcpy (&out->linkQuality, buf + o, 8); o += 8;
  return LINK_031_TLV_LEN;
}

uint32_t BuildLinkLoss032Payload (uint8_t* buf, uint32_t sz, const LinkLoss032& m)
{
  if (!buf || sz < LINK_LOSS_032_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_LINK_LOSS_032, LINK_LOSS_032_VALUE_LEN);
  uint32_t o = 3;
  buf[o++] = m.reporterNodeId;
  buf[o++] = m.peerCount;
  std::memcpy (buf + o, &m.lossPct, 8); o += 8;
  return LINK_LOSS_032_TLV_LEN;
}

uint32_t ParseLinkLoss032 (const uint8_t* buf, uint32_t len, LinkLoss032* out)
{
  if (!out || !Check (buf, len, TYPE_LINK_LOSS_032, LINK_LOSS_032_VALUE_LEN)) return 0;
  uint32_t o = 3;
  out->reporterNodeId = buf[o++];
  out->peerCount = buf[o++];
  std::memcpy (&out->lossPct, buf + o, 8); o += 8;
  return LINK_LOSS_032_TLV_LEN;
}

uint32_t BuildLinkStatus033Payload (uint8_t* buf, uint32_t sz, const LinkStatus033& m)
{
  if (!buf || sz < LINK_STATUS_033_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_LINK_STATUS_033, LINK_STATUS_033_VALUE_LEN);
  uint32_t o = 3;
  buf[o++] = m.reporterNodeId;
  buf[o++] = m.linkState;
  buf[o++] = m.upNeighbors;
  buf[o++] = m.reserve;
  return LINK_STATUS_033_TLV_LEN;
}

uint32_t ParseLinkStatus033 (const uint8_t* buf, uint32_t len, LinkStatus033* out)
{
  if (!out || !Check (buf, len, TYPE_LINK_STATUS_033, LINK_STATUS_033_VALUE_LEN)) return 0;
  uint32_t o = 3;
  out->reporterNodeId = buf[o++];
  out->linkState = buf[o++];
  out->upNeighbors = buf[o++];
  out->reserve = buf[o++];
  return LINK_STATUS_033_TLV_LEN;
}

uint32_t BuildLinkAgg034Payload (uint8_t* buf, uint32_t sz, const LinkAgg034& m)
{
  if (!buf || sz < LINK_AGG_034_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_LINK_AGG_034, LINK_AGG_034_VALUE_LEN);
  uint32_t o = 3;
  buf[o++] = m.subnetType;
  buf[o++] = m.reporterCount;
  buf[o++] = m.activeLinks;
  buf[o++] = m.upLinks;
  std::memcpy (buf + o, &m.avgLinkQuality, 8); o += 8;
  std::memcpy (buf + o, &m.avgLossPct, 8); o += 8;
  return LINK_AGG_034_TLV_LEN;
}

uint32_t ParseLinkAgg034 (const uint8_t* buf, uint32_t len, LinkAgg034* out)
{
  if (!out || !Check (buf, len, TYPE_LINK_AGG_034, LINK_AGG_034_VALUE_LEN)) return 0;
  uint32_t o = 3;
  out->subnetType = buf[o++];
  out->reporterCount = buf[o++];
  out->activeLinks = buf[o++];
  out->upLinks = buf[o++];
  std::memcpy (&out->avgLinkQuality, buf + o, 8); o += 8;
  std::memcpy (&out->avgLossPct, buf + o, 8); o += 8;
  return LINK_AGG_034_TLV_LEN;
}

uint32_t BuildAlert040Payload (uint8_t* buf, uint32_t sz, const Alert040& m)
{
  if (!buf || sz < ALERT_040_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_FAULT_040, ALERT_040_VALUE_LEN);
  uint32_t o = 3; buf[o++] = m.reportNodeId; buf[o++] = m.targetNodeId; buf[o++] = m.alertLevel; buf[o++] = m.alertReason;
  return ALERT_040_TLV_LEN;
}

uint32_t ParseAlert040 (const uint8_t* buf, uint32_t len, Alert040* out)
{
  if (!out || !Check (buf, len, TYPE_FAULT_040, ALERT_040_VALUE_LEN)) return 0;
  uint32_t o = 3; out->reportNodeId = buf[o++]; out->targetNodeId = buf[o++]; out->alertLevel = buf[o++]; out->alertReason = buf[o++];
  return ALERT_040_TLV_LEN;
}

uint32_t BuildRouteFail041Payload (uint8_t* buf, uint32_t sz, const RouteFail041& m)
{
  if (!buf || sz < ROUTE_FAIL_041_TLV_LEN) return 0;
  WriteTlvHeader (buf, TYPE_ROUTE_FAIL_041, ROUTE_FAIL_041_VALUE_LEN);
  uint32_t o = 3; buf[o++] = m.flowId; buf[o++] = m.faultNodeId; buf[o++] = m.failReason;
  return ROUTE_FAIL_041_TLV_LEN;
}

uint32_t ParseRouteFail041 (const uint8_t* buf, uint32_t len, RouteFail041* out)
{
  if (!out || !Check (buf, len, TYPE_ROUTE_FAIL_041, ROUTE_FAIL_041_VALUE_LEN)) return 0;
  uint32_t o = 3; out->flowId = buf[o++]; out->faultNodeId = buf[o++]; out->failReason = buf[o++];
  return ROUTE_FAIL_041_TLV_LEN;
}

uint32_t BuildLtePayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility)
{
  if (!buf || bufSize < MAX_LTE_PAYLOAD) return 0; WriteTlvHeader (buf, TYPE_TELEMETRY_010, 24);
  std::memcpy (buf + 3, &energy, 8); std::memcpy (buf + 11, &linkQ, 8); std::memcpy (buf + 19, &mobility, 8); return MAX_LTE_PAYLOAD;
}

uint32_t BuildAdhocPayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility)
{
  if (!buf || bufSize < MAX_ADHOC_PAYLOAD) return 0; WriteTlvHeader (buf, TYPE_TELEMETRY_010, 24);
  std::memcpy (buf + 3, &energy, 8); std::memcpy (buf + 11, &linkQ, 8); std::memcpy (buf + 19, &mobility, 8); return MAX_ADHOC_PAYLOAD;
}

uint32_t BuildDataLinkPayload (uint8_t* buf, uint32_t bufSize, double deltaEnergy)
{
  if (!buf || bufSize < LINK_031_TLV_LEN) return 0;
  Link031 link = {};
  link.reporterNodeId = 0;
  link.metricType = 1;
  link.linkQuality = deltaEnergy;
  return BuildLink031Payload (buf, bufSize, link);
}

uint32_t BuildPolicyPayload (uint8_t* buf, uint32_t maxLen)
{
  if (!buf || maxLen < SUBNET_012_TLV_LEN) return 0;
  Subnet012 subnet = {};
  subnet.reporterNodeId = 0;
  subnet.subnetType = 0;
  subnet.qosIntent = 1;
  subnet.reserve = 0;
  return BuildSubnet012Payload (buf, maxLen, subnet);
}

uint32_t BuildHelloElectionPayload (uint8_t* buf, uint32_t bufSize, uint32_t nodeId, double score)
{
  if (!buf || bufSize < HELLO_ELECTION_PAYLOAD) return 0; WriteTlvHeader (buf, TYPE_HELLO_ELECTION, 12);
  buf[3] = (nodeId >> 24) & 0xff; buf[4] = (nodeId >> 16) & 0xff; buf[5] = (nodeId >> 8) & 0xff; buf[6] = nodeId & 0xff; std::memcpy (buf + 7, &score, 8); return HELLO_ELECTION_PAYLOAD;
}

uint16_t ParseHelloElection (const uint8_t* buf, uint32_t len, uint32_t* outNodeId, double* outScore)
{
  if (!buf || len < 3) return 0; uint16_t valLen = R16 (buf + 1); if (buf[0] != TYPE_HELLO_ELECTION || len < 3u + valLen || valLen < 12) return 0;
  if (outNodeId) *outNodeId = R32 (buf + 3); if (outScore) std::memcpy (outScore, buf + 7, 8); return valLen;
}

uint32_t BuildNodeReportSpnPayload (uint8_t* buf, uint32_t bufSize, uint32_t nodeId, float px, float py, float pz, float vx, float vy, float vz, uint32_t timestampMs, uint8_t energyU8, uint8_t linkQU8, const std::vector<uint32_t>& neighborIds)
{
  uint16_t n = static_cast<uint16_t> (std::min (neighborIds.size (), (size_t) 64)); uint32_t valLen = 4 + 12 + 12 + 4 + 1 + 1 + 2 + n * 4; if (!buf || bufSize < 3u + valLen) return 0;
  WriteTlvHeader (buf, TYPE_NODE_REPORT_SPN, valLen); uint32_t off = 3; W32 (buf + off, nodeId); off += 4;
  std::memcpy (buf + off, &px, 4); off += 4; std::memcpy (buf + off, &py, 4); off += 4; std::memcpy (buf + off, &pz, 4); off += 4;
  std::memcpy (buf + off, &vx, 4); off += 4; std::memcpy (buf + off, &vy, 4); off += 4; std::memcpy (buf + off, &vz, 4); off += 4;
  W32 (buf + off, timestampMs); off += 4; buf[off++] = energyU8; buf[off++] = linkQU8; W16 (buf + off, n); off += 2;
  for (uint16_t i = 0; i < n; ++i) { W32 (buf + off, neighborIds[i]); off += 4; }
  return 3 + valLen;
}

uint32_t ParseNodeReportSpn (const uint8_t* val, uint16_t valLen, uint32_t* outNodeId, float* outPx, float* outPy, float* outPz, float* outVx, float* outVy, float* outVz, uint32_t* outTimestampMs, uint8_t* outEnergyU8, uint8_t* outLinkQU8, std::vector<uint32_t>* outNeighborIds)
{
  if (!val || valLen < 4 + 12 + 12 + 4 + 1 + 1 + 2) return 0; uint32_t off = 0; if (outNodeId) *outNodeId = R32 (val + off); off += 4;
  if (outPx) std::memcpy (outPx, val + off, 4); off += 4; if (outPy) std::memcpy (outPy, val + off, 4); off += 4; if (outPz) std::memcpy (outPz, val + off, 4); off += 4;
  if (outVx) std::memcpy (outVx, val + off, 4); off += 4; if (outVy) std::memcpy (outVy, val + off, 4); off += 4; if (outVz) std::memcpy (outVz, val + off, 4); off += 4;
  if (outTimestampMs) *outTimestampMs = R32 (val + off); off += 4; if (outEnergyU8) *outEnergyU8 = val[off++]; if (outLinkQU8) *outLinkQU8 = val[off++]; uint16_t n = R16 (val + off); off += 2;
  if (outNeighborIds) { outNeighborIds->clear (); for (uint16_t i = 0; i < n && off + 4 <= valLen; ++i) { outNeighborIds->push_back (R32 (val + off)); off += 4; } } else off += n * 4;
  return off;
}

uint32_t BuildScoreFloodPayload (uint8_t* buf, uint32_t bufSize, uint8_t ttl, const std::vector<std::pair<uint32_t, double>>& entries)
{
  uint16_t n = static_cast<uint16_t> (std::min (entries.size (), (size_t) MAX_SCORE_FLOOD_ENTRIES)); uint32_t valLen = 3 + n * 12; if (!buf || bufSize < 3u + valLen) return 0;
  WriteTlvHeader (buf, TYPE_SCORE_FLOOD, valLen); buf[3] = ttl; W16 (buf + 4, n); uint32_t off = 6;
  for (uint16_t i = 0; i < n; ++i) { W32 (buf + off, entries[i].first); off += 4; std::memcpy (buf + off, &entries[i].second, 8); off += 8; }
  return 3 + valLen;
}

uint32_t ParseScoreFlood (const uint8_t* buf, uint32_t len, uint8_t* outTtl, std::vector<std::pair<uint32_t, double>>* outEntries)
{
  if (!buf || len < 6 || buf[0] != TYPE_SCORE_FLOOD) return 0; uint16_t valLen = R16 (buf + 1); if (len < 3u + valLen || valLen < 3) return 0;
  if (outTtl) *outTtl = buf[3]; uint16_t n = R16 (buf + 4); uint32_t off = 6;
  if (outEntries) { outEntries->clear (); for (uint16_t i = 0; i < n && off + 12 <= 3u + valLen; ++i) { uint32_t id = R32 (buf + off); double sc = 0.0; std::memcpy (&sc, buf + off + 4, 8); outEntries->push_back (std::make_pair (id, sc)); off += 12; } }
  else off = 6 + n * 12;
  return 3 + valLen;
}

uint32_t BuildHeartbeatSyncPayload (uint8_t* buf, uint32_t bufSize, uint32_t primaryId, double nowSec)
{
  if (!buf || bufSize < 15) return 0; WriteTlvHeader (buf, TYPE_HEARTBEAT_SYNC, 12); W32 (buf + 3, primaryId); std::memcpy (buf + 7, &nowSec, 8); return 15;
}

uint32_t ParseHeartbeatSyncPayload (const uint8_t* buf, uint32_t len, uint32_t* outPrimaryId, double* outNowSec)
{
  if (!buf || len < 15 || buf[0] != TYPE_HEARTBEAT_SYNC) return 0; uint16_t valLen = R16 (buf + 1); if (valLen < 12 || len < 3u + valLen) return 0;
  if (outPrimaryId) *outPrimaryId = R32 (buf + 3); if (outNowSec) std::memcpy (outNowSec, buf + 7, 8); return 3 + valLen;
}
} // namespace NmsTlv
