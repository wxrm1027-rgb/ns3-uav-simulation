#ifndef HETEROGENEOUS_NMS_JSON_CONFIG_H
#define HETEROGENEOUS_NMS_JSON_CONFIG_H

#include "types.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct NodeJoinConfig
{
  uint32_t nodeId;
  std::string type;
  std::string subnet;
  double joinTime;
  double initPos[3];
  double speed;
  std::string ipAddress;
  double initialRateMbps;
  double initialEnergy;
  double initialEnergyMah;
  double initialLinkQuality;
  std::vector<uint32_t> neighbors;
};

std::map<uint32_t, NodeJoinConfig> LoadNodeJoinConfig (const std::string& path);
std::string ValidateJoinConfig (const std::map<uint32_t, NodeJoinConfig>& config);
ScenarioConfig LoadScenarioConfig (const std::string& path);

#endif // HETEROGENEOUS_NMS_JSON_CONFIG_H

