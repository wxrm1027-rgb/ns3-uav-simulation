#include "tlv.h"

#include <algorithm>
#include <cstring>

namespace NmsTlv
{
void WriteTlvHeader (uint8_t* buf, uint8_t type, uint16_t length)
{
  if (!buf) return;
  buf[0] = type;
  buf[1] = (length >> 8) & 0xff;
  buf[2] = length & 0xff;
}

uint32_t BuildLtePayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility)
{
  if (!buf || bufSize < MAX_LTE_PAYLOAD) return 0;
  WriteTlvHeader (buf, TYPE_TELEMETRY_010, 24);
  std::memcpy (buf + 3, &energy, 8);
  std::memcpy (buf + 11, &linkQ, 8);
  std::memcpy (buf + 19, &mobility, 8);
  return MAX_LTE_PAYLOAD;
}

uint32_t BuildAdhocPayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility)
{
  if (!buf || bufSize < MAX_ADHOC_PAYLOAD) return 0;
  WriteTlvHeader (buf, TYPE_TELEMETRY_010, 24);
  std::memcpy (buf + 3, &energy, 8);
  std::memcpy (buf + 11, &linkQ, 8);
  std::memcpy (buf + 19, &mobility, 8);
  return MAX_ADHOC_PAYLOAD;
}

uint32_t BuildDataLinkPayload (uint8_t* buf, uint32_t bufSize, double deltaEnergy)
{
  if (!buf || bufSize < MAX_DATALINK_PAYLOAD) return 0;
  WriteTlvHeader (buf, TYPE_LINK_031, 8);
  std::memcpy (buf + 3, &deltaEnergy, 8);
  return MAX_DATALINK_PAYLOAD;
}

uint32_t BuildPolicyPayload (uint8_t* buf, uint32_t maxLen)
{
  if (!buf || maxLen < 4) return 0;
  const char policy[] = "POLICY";
  uint16_t len = static_cast<uint16_t> (std::min (sizeof (policy) - 1, (size_t) maxLen));
  WriteTlvHeader (buf, TYPE_SUBNET_012, len);
  std::memcpy (buf + 3, policy, len);
  return 3 + len;
}

uint32_t BuildHelloElectionPayload (uint8_t* buf, uint32_t bufSize, uint32_t nodeId, double score)
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

uint16_t ParseHelloElection (const uint8_t* buf, uint32_t len, uint32_t* outNodeId, double* outScore)
{
  if (!buf || len < 3) return 0;
  uint16_t valLen = (static_cast<uint16_t> (buf[1]) << 8) | buf[2];
  if (buf[0] != TYPE_HELLO_ELECTION || len < 3u + valLen || valLen < 12) return 0;
  if (outNodeId)
    *outNodeId = (static_cast<uint32_t> (buf[3]) << 24) | (static_cast<uint32_t> (buf[4]) << 16)
                 | (static_cast<uint32_t> (buf[5]) << 8) | buf[6];
  if (outScore) std::memcpy (outScore, buf + 7, 8);
  return valLen;
}

uint32_t BuildNodeReportSpnPayload (uint8_t* buf, uint32_t bufSize,
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

uint32_t ParseNodeReportSpn (const uint8_t* val, uint16_t valLen,
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
  else off += n * 4;
  return off;
}

uint32_t BuildScoreFloodPayload (uint8_t* buf, uint32_t bufSize, uint8_t ttl,
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

uint32_t ParseScoreFlood (const uint8_t* buf, uint32_t len,
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
  else off = 6 + n * 12;
  return 3 + valLen;
}

uint32_t BuildHeartbeatSyncPayload (uint8_t* buf, uint32_t bufSize, uint32_t primaryId, double nowSec)
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

uint32_t ParseHeartbeatSyncPayload (const uint8_t* buf, uint32_t len, uint32_t* outPrimaryId, double* outNowSec)
{
  if (!buf || len < 15 || buf[0] != TYPE_HEARTBEAT_SYNC) return 0;
  uint16_t valLen = (static_cast<uint16_t> (buf[1]) << 8) | buf[2];
  if (valLen < 12 || len < 3u + valLen) return 0;
  if (outPrimaryId)
    *outPrimaryId = (static_cast<uint32_t> (buf[3]) << 24) | (static_cast<uint32_t> (buf[4]) << 16)
                  | (static_cast<uint32_t> (buf[5]) << 8) | buf[6];
  if (outNowSec) std::memcpy (outNowSec, buf + 7, 8);
  return 3 + valLen;
}
} // namespace NmsTlv

