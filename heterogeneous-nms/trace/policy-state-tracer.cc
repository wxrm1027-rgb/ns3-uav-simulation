#include "policy-state-tracer.h"

#include "ns3/core-module.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>

namespace hnms
{
namespace
{
std::ofstream g_policyStateTraceFile;
bool g_policyStateTraceInitialized = false;
std::map<uint32_t, double> g_mirroredTxPowerByNode;
std::mutex g_policyStateTraceMutex;

std::string
EscapeCsv (const std::string& input)
{
  bool needQuotes = false;
  for (char c : input)
    {
      if (c == ',' || c == '"' || c == '\n')
        {
          needQuotes = true;
          break;
        }
    }

  if (!needQuotes)
    {
      return input;
    }

  std::string out = "\"";
  for (char c : input)
    {
      if (c == '"')
        {
          out += "\"\"";
        }
      else
        {
          out += c;
        }
    }
  out += "\"";
  return out;
}

std::string
FormatDouble (double value)
{
  if (!std::isfinite (value))
    {
      return "nan";
    }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision (6) << value;
  return oss.str ();
}

} // namespace

void
PolicyStateTracer::Init (const std::string& fileName)
{
  std::lock_guard<std::mutex> lock (g_policyStateTraceMutex);
  if (g_policyStateTraceFile.is_open ())
    {
      g_policyStateTraceFile.close ();
    }

  g_policyStateTraceFile.open (fileName.c_str (), std::ios::out | std::ios::trunc);
  g_policyStateTraceInitialized = g_policyStateTraceFile.is_open ();
  g_mirroredTxPowerByNode.clear ();

  if (g_policyStateTraceInitialized)
    {
      g_policyStateTraceFile
          << "time,nodeId,subnet,intentType,stage,oldTxPower,newTxPower,hasWifi,selected,success,errorCode,reason"
          << std::endl;
    }
}

void
PolicyStateTracer::Log (const PolicyStateTraceRecord& record)
{
  std::lock_guard<std::mutex> lock (g_policyStateTraceMutex);
  if (!g_policyStateTraceInitialized || !g_policyStateTraceFile.is_open ())
    {
      return;
    }

  g_policyStateTraceFile
      << FormatDouble (record.time) << ","
      << record.nodeId << ","
      << EscapeCsv (record.subnet) << ","
      << static_cast<uint32_t> (record.intentType) << ","
      << EscapeCsv (record.stage) << ","
      << FormatDouble (record.oldTxPower) << ","
      << FormatDouble (record.newTxPower) << ","
      << (record.hasWifi ? 1 : 0) << ","
      << (record.selected ? 1 : 0) << ","
      << (record.success ? 1 : 0) << ","
      << record.errorCode << ","
      << EscapeCsv (record.reason)
      << std::endl;
}

void
PolicyStateTracer::Close ()
{
  std::lock_guard<std::mutex> lock (g_policyStateTraceMutex);
  if (g_policyStateTraceFile.is_open ())
    {
      g_policyStateTraceFile.flush ();
      g_policyStateTraceFile.close ();
    }
  g_policyStateTraceInitialized = false;
}

bool
PolicyStateTracer::IsInitialized ()
{
  std::lock_guard<std::mutex> lock (g_policyStateTraceMutex);
  return g_policyStateTraceInitialized;
}

void
PolicyStateTracer::UpdateMirroredTxPower (uint32_t nodeId, double txPowerDbm)
{
  std::lock_guard<std::mutex> lock (g_policyStateTraceMutex);
  g_mirroredTxPowerByNode[nodeId] = txPowerDbm;
}

double
PolicyStateTracer::GetMirroredTxPower (uint32_t nodeId, bool* hasValue)
{
  std::lock_guard<std::mutex> lock (g_policyStateTraceMutex);
  auto it = g_mirroredTxPowerByNode.find (nodeId);
  if (it == g_mirroredTxPowerByNode.end ())
    {
      if (hasValue)
        {
          *hasValue = false;
        }
      return std::numeric_limits<double>::quiet_NaN ();
    }
  if (hasValue)
    {
      *hasValue = true;
    }
  return it->second;
}

} // namespace hnms
