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
static constexpr uint8_t TYPE_INTENT_013     = 0x13;
static constexpr uint8_t TYPE_EXEC_CMD_014   = 0x14;
static constexpr uint8_t TYPE_EXEC_RESULT_015 = 0x15;
static constexpr uint8_t TYPE_INTENT_REPORT_016 = 0x16;
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

static constexpr uint32_t TELEMETRY_010_VALUE_LEN = 26;
static constexpr uint32_t ROLE_011_VALUE_LEN = 3;
static constexpr uint32_t INTENT_013_VALUE_LEN = 7;
static constexpr uint32_t EXEC_CMD_014_VALUE_LEN = 8;
static constexpr uint32_t EXEC_RESULT_015_VALUE_LEN = 4;
static constexpr uint32_t INTENT_REPORT_016_VALUE_LEN = 4;
static constexpr uint32_t ALERT_040_VALUE_LEN = 4;
static constexpr uint32_t ROUTE_FAIL_041_VALUE_LEN = 3;

static constexpr uint32_t TELEMETRY_010_TLV_LEN = 3 + TELEMETRY_010_VALUE_LEN;
static constexpr uint32_t ROLE_011_TLV_LEN = 3 + ROLE_011_VALUE_LEN;
static constexpr uint32_t INTENT_013_TLV_LEN = 3 + INTENT_013_VALUE_LEN;
static constexpr uint32_t EXEC_CMD_014_TLV_LEN = 3 + EXEC_CMD_014_VALUE_LEN;
static constexpr uint32_t EXEC_RESULT_015_TLV_LEN = 3 + EXEC_RESULT_015_VALUE_LEN;
static constexpr uint32_t INTENT_REPORT_016_TLV_LEN = 3 + INTENT_REPORT_016_VALUE_LEN;
static constexpr uint32_t ALERT_040_TLV_LEN = 3 + ALERT_040_VALUE_LEN;
static constexpr uint32_t ROUTE_FAIL_041_TLV_LEN = 3 + ROUTE_FAIL_041_VALUE_LEN;

static constexpr uint32_t MAX_LTE_PAYLOAD = 3 + 24;
static constexpr uint32_t MAX_ADHOC_PAYLOAD = 3 + 24;
static constexpr uint32_t MAX_DATALINK_PAYLOAD = 3 + 8;
static constexpr uint32_t MAX_POLICY_PAYLOAD = 32;
static constexpr uint32_t HELLO_ELECTION_PAYLOAD = 3 + 4 + 8;
static constexpr uint32_t MAX_NODE_REPORT_PAYLOAD = 3 + 36 + 64 * 4;
static constexpr uint32_t MAX_SCORE_FLOOD_ENTRIES = 50;
static constexpr uint32_t MAX_SCORE_FLOOD_PAYLOAD = 3 + 3 + MAX_SCORE_FLOOD_ENTRIES * 12;

struct Telemetry010
{
  uint8_t nodeId;
  uint8_t batteryLevel;
  float posX;
  float posY;
  float posZ;
  float vx;
  float vy;
  float vz;
};

struct Role011
{
  uint8_t nodeId;
  uint8_t roleType;
  uint8_t computeLevel;
};

struct Intent013
{
  uint8_t intentId;
  uint8_t intentType;
  uint16_t validTime;
  uint16_t param1;
  uint8_t param2;
};

struct ExecCmd014
{
  uint8_t intentId;
  uint8_t cmdSeq;
  uint8_t targetNodeId;
  uint8_t cmdType;
  uint32_t cmdParam;
};

struct ExecResult015
{
  uint8_t intentId;
  uint8_t cmdSeq;
  uint8_t execResult;
  uint8_t failReason;
};

struct IntentReport016
{
  uint8_t intentId;
  uint8_t totalTargets;
  uint8_t successCount;
  uint8_t failCount;
};

struct Alert040
{
  uint8_t reportNodeId;
  uint8_t targetNodeId;
  uint8_t alertLevel;
  uint8_t alertReason;
};

struct RouteFail041
{
  uint8_t flowId;
  uint8_t faultNodeId;
  uint8_t failReason;
};

void WriteTlvHeader (uint8_t* buf, uint8_t type, uint16_t length);

uint32_t BuildTelemetry010Payload (uint8_t* buf, uint32_t bufSize, const Telemetry010& m);
uint32_t ParseTelemetry010 (const uint8_t* buf, uint32_t len, Telemetry010* out);
uint32_t BuildRole011Payload (uint8_t* buf, uint32_t bufSize, const Role011& m);
uint32_t ParseRole011 (const uint8_t* buf, uint32_t len, Role011* out);
uint32_t BuildIntent013Payload (uint8_t* buf, uint32_t bufSize, const Intent013& m);
uint32_t ParseIntent013 (const uint8_t* buf, uint32_t len, Intent013* out);
uint32_t BuildExecCmd014Payload (uint8_t* buf, uint32_t bufSize, const ExecCmd014& m);
uint32_t ParseExecCmd014 (const uint8_t* buf, uint32_t len, ExecCmd014* out);
uint32_t BuildExecResult015Payload (uint8_t* buf, uint32_t bufSize, const ExecResult015& m);
uint32_t ParseExecResult015 (const uint8_t* buf, uint32_t len, ExecResult015* out);
uint32_t BuildIntentReport016Payload (uint8_t* buf, uint32_t bufSize, const IntentReport016& m);
uint32_t ParseIntentReport016 (const uint8_t* buf, uint32_t len, IntentReport016* out);
uint32_t BuildAlert040Payload (uint8_t* buf, uint32_t bufSize, const Alert040& m);
uint32_t ParseAlert040 (const uint8_t* buf, uint32_t len, Alert040* out);
uint32_t BuildRouteFail041Payload (uint8_t* buf, uint32_t bufSize, const RouteFail041& m);
uint32_t ParseRouteFail041 (const uint8_t* buf, uint32_t len, RouteFail041* out);

uint32_t BuildLtePayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility);
uint32_t BuildAdhocPayload (uint8_t* buf, uint32_t bufSize, double energy, double linkQ, double mobility);
uint32_t BuildDataLinkPayload (uint8_t* buf, uint32_t bufSize, double deltaEnergy);
uint32_t BuildPolicyPayload (uint8_t* buf, uint32_t maxLen);
uint32_t BuildHelloElectionPayload (uint8_t* buf, uint32_t bufSize, uint32_t nodeId, double score);
uint16_t ParseHelloElection (const uint8_t* buf, uint32_t len, uint32_t* outNodeId, double* outScore);
uint32_t BuildNodeReportSpnPayload (uint8_t* buf, uint32_t bufSize,
                                    uint32_t nodeId, float px, float py, float pz,
                                    float vx, float vy, float vz,
                                    uint32_t timestampMs,
                                    uint8_t energyU8, uint8_t linkQU8,
                                    const std::vector<uint32_t>& neighborIds);
uint32_t ParseNodeReportSpn (const uint8_t* val, uint16_t valLen,
                             uint32_t* outNodeId, float* outPx, float* outPy, float* outPz,
                             float* outVx, float* outVy, float* outVz,
                             uint32_t* outTimestampMs,
                             uint8_t* outEnergyU8, uint8_t* outLinkQU8,
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
