#ifndef HETEROGENEOUS_NMS_TLV_H
#define HETEROGENEOUS_NMS_TLV_H

#include <cstdint>
#include <utility>
#include <vector>

namespace NmsTlv
{
static constexpr uint8_t TYPE_TELEMETRY_010  = 0x10;
static constexpr uint8_t TYPE_ROLE_011       = 0x11;
static constexpr uint8_t TYPE_SUBNET_012     = 0x12;
static constexpr uint8_t TYPE_FLOW_020       = 0x20;
static constexpr uint8_t TYPE_TOPO_030       = 0x30;
static constexpr uint8_t TYPE_LINK_031       = 0x31;
static constexpr uint8_t TYPE_FAULT_040      = 0x40;
static constexpr uint8_t TYPE_ROUTE_FAIL_041 = 0x41;
static constexpr uint8_t TYPE_HELLO_ELECTION = 0x80;
static constexpr uint8_t TYPE_NODE_REPORT_SPN = 0x81;
static constexpr uint8_t TYPE_TOPOLOGY_AGGREGATE = 0x82;
static constexpr uint8_t TYPE_SCORE_FLOOD = 0x83;
static constexpr uint8_t TYPE_HEARTBEAT_SYNC = 0x84;

static constexpr uint32_t MAX_LTE_PAYLOAD = 3 + 24;
static constexpr uint32_t MAX_ADHOC_PAYLOAD = 3 + 24;
static constexpr uint32_t MAX_DATALINK_PAYLOAD = 3 + 8;
static constexpr uint32_t MAX_POLICY_PAYLOAD = 32;
static constexpr uint32_t HELLO_ELECTION_PAYLOAD = 3 + 4 + 8;
static constexpr uint32_t MAX_NODE_REPORT_PAYLOAD = 3 + 4 + 24 + 12 + 8 + 8 + 2 + 64 * 4;
static constexpr uint32_t MAX_SCORE_FLOOD_ENTRIES = 50;
static constexpr uint32_t MAX_SCORE_FLOOD_PAYLOAD = 3 + 3 + MAX_SCORE_FLOOD_ENTRIES * 12;

void WriteTlvHeader (uint8_t* buf, uint8_t type, uint16_t length);
uint32_t BuildLtePayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility);
uint32_t BuildAdhocPayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility);
uint32_t BuildDataLinkPayload (uint8_t* buf, uint32_t bufSize, double deltaEnergy);
uint32_t BuildPolicyPayload (uint8_t* buf, uint32_t maxLen);
uint32_t BuildHelloElectionPayload (uint8_t* buf, uint32_t bufSize, uint32_t nodeId, double score);
uint16_t ParseHelloElection (const uint8_t* buf, uint32_t len, uint32_t* outNodeId, double* outScore);
uint32_t BuildNodeReportSpnPayload (uint8_t* buf, uint32_t bufSize,
                                    uint32_t nodeId, double px, double py, double pz,
                                    float vx, float vy, float vz,
                                    double energy, double linkQ,
                                    const std::vector<uint32_t>& neighborIds);
uint32_t ParseNodeReportSpn (const uint8_t* val, uint16_t valLen,
                             uint32_t* outNodeId, double* outPx, double* outPy, double* outPz,
                             float* outVx, float* outVy, float* outVz,
                             double* outEnergy, double* outLinkQ,
                             std::vector<uint32_t>* outNeighborIds);
uint32_t BuildScoreFloodPayload (uint8_t* buf, uint32_t bufSize, uint8_t ttl,
                                 const std::vector<std::pair<uint32_t, double>>& entries);
uint32_t ParseScoreFlood (const uint8_t* buf, uint32_t len,
                          uint8_t* outTtl,
                          std::vector<std::pair<uint32_t, double>>* outEntries);
uint32_t BuildHeartbeatSyncPayload (uint8_t* buf, uint32_t bufSize, uint32_t primaryId, double nowSec);
uint32_t ParseHeartbeatSyncPayload (const uint8_t* buf, uint32_t len, uint32_t* outPrimaryId, double* outNowSec);
} // namespace NmsTlv

#endif // HETEROGENEOUS_NMS_TLV_H

