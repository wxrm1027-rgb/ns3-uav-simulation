#ifndef HETEROGENEOUS_NMS_LOGGER_H
#define HETEROGENEOUS_NMS_LOGGER_H

#include <fstream>
#include <cstdint>
#include <string>

extern std::ofstream g_nmsLogFile;
void NmsLog (const char* level, uint32_t nodeId, const char* eventType, const std::string& details);

#endif // HETEROGENEOUS_NMS_LOGGER_H

