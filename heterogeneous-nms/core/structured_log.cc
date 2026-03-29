#include "structured_log.h"

#include "logger.h"

#include "ns3/core-module.h"

#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

using namespace ns3;

namespace hnms
{

namespace
{
std::string
JsonEscape (const std::string& s)
{
  std::string o;
  o.reserve (s.size () + 8);
  for (char c : s)
    {
      if (c == '"' || c == '\\')
        o += '\\';
      o += c;
    }
  return o;
}
} // namespace

bool StructuredLog::s_json{false};

std::string
StructuredLog::NewTraceId ()
{
  static thread_local std::mt19937_64 rng (
      std::random_device {} ());
  std::ostringstream o;
  o << std::hex << rng ();
  return o.str ();
}

const char*
StructuredLog::LevelStr (LogLevel l)
{
  switch (l)
    {
    case LogLevel::DEBUG:
      return "DEBUG";
    case LogLevel::INFO:
      return "INFO";
    case LogLevel::WARN:
      return "WARN";
    case LogLevel::ERROR:
      return "ERROR";
    default:
      return "INFO";
    }
}

void
StructuredLog::Write (LogLevel level, std::uint32_t nodeId, const std::string& eventType,
                    const std::string& traceId, const std::string& msg)
{
  double ts = Simulator::Now ().GetSeconds ();
  if (s_json)
    {
      std::ostringstream json;
      json << std::fixed << std::setprecision (6) << "{\"timestamp\":" << ts << ",\"level\":\""
           << LevelStr (level) << "\",\"node_id\":" << nodeId << ",\"event\":\"" << JsonEscape (eventType)
           << "\",\"trace_id\":\"" << JsonEscape (traceId) << "\",\"msg\":\"" << JsonEscape (msg) << "\"}";
      std::string line = json.str ();
      if (g_nmsLogFile.is_open ())
        {
          g_nmsLogFile << line << std::endl;
        }
      else
        {
          std::cout << line << std::endl;
        }
      return;
    }
  const char* l = "INFO";
  if (level == LogLevel::ERROR)
    l = "ERROR";
  else if (level == LogLevel::WARN)
    l = "WARN";
  NmsLog (l, nodeId, eventType.c_str (), msg);
}

} // namespace hnms
