#include "logger.h"

#include "ns3/core-module.h"

#include <cstring>
#include <iomanip>
#include <sstream>

using namespace ns3;

// 与 heterogeneous-nms-framework.cc 中的 HeterogeneousNodeApp 组件分离，避免模块化链接时重复注册。
NS_LOG_COMPONENT_DEFINE ("HnmsStructuredLog");

void
NmsLog (const char* level, uint32_t nodeId, const char* eventType, const std::string& details)
{
  double t = Simulator::Now ().GetSeconds ();
  std::ostringstream line;
  line << std::fixed << std::setprecision (3) << "[" << t << "s] [Node " << nodeId << "] ["
       << eventType << "] " << details;
  std::string s = line.str ();
  if (g_nmsLogFile.is_open ())
    {
      g_nmsLogFile << s << std::endl;
    }
  if (std::strcmp (level, "ERROR") == 0)
    {
      NS_LOG_ERROR (s);
    }
  else if (std::strcmp (level, "WARN") == 0)
    {
      NS_LOG_WARN (s);
    }
  else
    {
      NS_LOG_INFO (s);
    }
}

