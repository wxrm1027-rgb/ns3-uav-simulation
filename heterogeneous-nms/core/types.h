#ifndef HETEROGENEOUS_NMS_TYPES_H
#define HETEROGENEOUS_NMS_TYPES_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct TrajectoryPoint
{
  double t;
  double x;
  double y;
  double z;
};

struct ScenarioEvent
{
  double time;
  std::string type;
  uint32_t target;
  uint32_t triggerNodeId;
  uint32_t newSpnNodeId;
};

struct BusinessFlowConfig
{
  uint32_t flowId;
  std::string type;
  uint8_t priority;
  uint8_t qos;
  uint32_t srcNodeId;
  uint32_t dstNodeId;
  std::string dataRate;
  uint32_t packetSize;
  double startTime;
  double stopTime;
};

struct ScenarioConfig
{
  std::string scenarioId;
  std::map<uint32_t, std::vector<TrajectoryPoint>> trajectoryByNode;
  std::vector<std::string> eventRules;
  std::vector<ScenarioEvent> events;
  std::vector<BusinessFlowConfig> businessFlows;
  double spnElectionTimeoutSec;
  uint32_t spnHeartbeatMissThreshold;
};

#endif // HETEROGENEOUS_NMS_TYPES_H

