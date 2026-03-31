#include "config_loader.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hnms
{

ConfigLoader::Defaults ConfigLoader::s_val{};
std::string ConfigLoader::s_path;

/// 在 section 子串内查找 "key": number（简单场景：spn 块内第一个匹配）
static double
ParseInSection (const std::string& content, const char* sectionName, const char* field, double defv)
{
  size_t sec = content.find (sectionName);
  if (sec == std::string::npos)
    {
      return defv;
    }
  size_t brace = content.find ('{', sec);
  if (brace == std::string::npos)
    {
      return defv;
    }
  size_t end = content.find ('}', brace);
  if (end == std::string::npos)
    {
      return defv;
    }
  std::string sub = content.substr (brace, end - brace + 1);
  std::string q = std::string ("\"") + field + "\"";
  size_t f = sub.find (q);
  if (f == std::string::npos)
    {
      return defv;
    }
  size_t colon = sub.find (':', f);
  if (colon == std::string::npos)
    {
      return defv;
    }
  return std::strtod (sub.c_str () + colon + 1, nullptr);
}

static std::string
ParseStringInSection (const std::string& content, const char* sectionName, const char* field, const std::string& defv)
{
  size_t sec = content.find (sectionName);
  if (sec == std::string::npos)
    {
      return defv;
    }
  size_t brace = content.find ('{', sec);
  if (brace == std::string::npos)
    {
      return defv;
    }
  size_t end = content.find ('}', brace);
  if (end == std::string::npos)
    {
      return defv;
    }
  std::string sub = content.substr (brace, end - brace + 1);
  std::string q = std::string ("\"") + field + "\"";
  size_t f = sub.find (q);
  if (f == std::string::npos)
    {
      return defv;
    }
  size_t colon = sub.find (':', f);
  if (colon == std::string::npos)
    {
      return defv;
    }
  size_t firstQuote = sub.find ('"', colon + 1);
  if (firstQuote == std::string::npos)
    {
      return defv;
    }
  size_t secondQuote = sub.find ('"', firstQuote + 1);
  if (secondQuote == std::string::npos)
    {
      return defv;
    }
  return sub.substr (firstQuote + 1, secondQuote - firstQuote - 1);
}

void
ConfigLoader::LoadFromFile (const std::string& path)
{
  s_path = path;
  if (path.empty ())
    {
      return;
    }
  std::ifstream f (path.c_str ());
  if (!f)
    {
      return;
    }
  std::stringstream ss;
  ss << f.rdbuf ();
  std::string content = ss.str ();

  s_val.spn_election_wait = ParseInSection (content, "\"spn\"", "election_wait_time", s_val.spn_election_wait);
  s_val.spn_hello = ParseInSection (content, "\"spn\"", "hello_interval", s_val.spn_hello);
  s_val.spn_suppress = ParseInSection (content, "\"spn\"", "suppress_window", s_val.spn_suppress);
  s_val.spn_heartbeat_miss = static_cast<std::uint32_t> (
      ParseInSection (content, "\"spn\"", "heartbeat_miss_threshold",
                     static_cast<double> (s_val.spn_heartbeat_miss)) +
      0.5);

  s_val.node_batt_low = ParseInSection (content, "\"node\"", "battery_threshold_low", s_val.node_batt_low);
  s_val.node_speed_max = ParseInSection (content, "\"node\"", "speed_max", s_val.node_speed_max);

  s_val.link_snr_conn = ParseInSection (content, "\"link\"", "snr_threshold_connect", s_val.link_snr_conn);
  s_val.link_snr_disconn = ParseInSection (content, "\"link\"", "snr_threshold_disconnect", s_val.link_snr_disconn);

  s_val.qos_lat = ParseInSection (content, "\"qos\"", "max_latency_ms", s_val.qos_lat);
  s_val.qos_tp = ParseInSection (content, "\"qos\"", "min_throughput_mbps", s_val.qos_tp);

  s_val.sim_time = ParseInSection (content, "\"simulation\"", "total_time", s_val.sim_time);
  s_val.sim_nodes = static_cast<std::uint32_t> (
      ParseInSection (content, "\"simulation\"", "num_nodes", static_cast<double> (s_val.sim_nodes)) + 0.5);
  s_val.sim_join_config =
      ParseStringInSection (content, "\"simulation\"", "join_config", s_val.sim_join_config);
  s_val.sim_scenario_config =
      ParseStringInSection (content, "\"simulation\"", "scenario_config", s_val.sim_scenario_config);
  s_val.sim_scenario_mode =
      ParseStringInSection (content, "\"simulation\"", "scenario_mode", s_val.sim_scenario_mode);
  s_val.sim_parse_packets = static_cast<std::uint32_t> (
      ParseInSection (content, "\"simulation\"", "parse_packets", static_cast<double> (s_val.sim_parse_packets)) + 0.5);
  s_val.sim_pcap = static_cast<std::uint32_t> (
      ParseInSection (content, "\"simulation\"", "pcap", static_cast<double> (s_val.sim_pcap)) + 0.5);
  s_val.sim_dual_channel = static_cast<std::uint32_t> (
      ParseInSection (content, "\"simulation\"", "dual_channel", static_cast<double> (s_val.sim_dual_channel)) + 0.5);
  s_val.sim_energy_delta_th =
      ParseInSection (content, "\"simulation\"", "energy_delta_th", s_val.sim_energy_delta_th);
  s_val.sim_state_suppress_win =
      ParseInSection (content, "\"simulation\"", "state_suppress_win", s_val.sim_state_suppress_win);
  s_val.sim_aggregate_interval =
      ParseInSection (content, "\"simulation\"", "aggregate_interval", s_val.sim_aggregate_interval);
  s_val.sim_route_mode = ParseStringInSection (content, "\"simulation\"", "route_mode", s_val.sim_route_mode);
  s_val.sim_route_adapt = ParseStringInSection (content, "\"simulation\"", "route_adapt", s_val.sim_route_adapt);
  s_val.sim_route_adapt_runtime = static_cast<std::uint32_t> (
      ParseInSection (content, "\"simulation\"", "route_adapt_runtime", static_cast<double> (s_val.sim_route_adapt_runtime)) + 0.5);
  s_val.sim_route_adapt_window =
      ParseInSection (content, "\"simulation\"", "route_adapt_runtime_window", s_val.sim_route_adapt_window);
  s_val.sim_route_adapt_cooldown =
      ParseInSection (content, "\"simulation\"", "route_adapt_runtime_cooldown", s_val.sim_route_adapt_cooldown);
}

std::string
ConfigLoader::LastPath ()
{
  return s_path;
}

double
ConfigLoader::SpnElectionWaitTime ()
{
  return s_val.spn_election_wait;
}
double
ConfigLoader::SpnHelloInterval ()
{
  return s_val.spn_hello;
}
double
ConfigLoader::SpnSuppressWindow ()
{
  return s_val.spn_suppress;
}
std::uint32_t
ConfigLoader::SpnHeartbeatMissThreshold ()
{
  return s_val.spn_heartbeat_miss;
}
double
ConfigLoader::NodeBatteryLow ()
{
  return s_val.node_batt_low;
}
double
ConfigLoader::NodeSpeedMax ()
{
  return s_val.node_speed_max;
}
double
ConfigLoader::LinkSnrConnect ()
{
  return s_val.link_snr_conn;
}
double
ConfigLoader::LinkSnrDisconnect ()
{
  return s_val.link_snr_disconn;
}
double
ConfigLoader::QosMaxLatencyMs ()
{
  return s_val.qos_lat;
}
double
ConfigLoader::QosMinThroughputMbps ()
{
  return s_val.qos_tp;
}
double
ConfigLoader::SimulationTotalTime ()
{
  return s_val.sim_time;
}
std::uint32_t
ConfigLoader::SimulationNumNodes ()
{
  return s_val.sim_nodes;
}

std::string
ConfigLoader::SimulationJoinConfig ()
{
  return s_val.sim_join_config;
}

std::string
ConfigLoader::SimulationScenarioConfig ()
{
  return s_val.sim_scenario_config;
}

std::string
ConfigLoader::SimulationScenarioMode ()
{
  return s_val.sim_scenario_mode;
}

std::uint32_t
ConfigLoader::SimulationParsePackets ()
{
  return s_val.sim_parse_packets;
}

std::uint32_t
ConfigLoader::SimulationPcap ()
{
  return s_val.sim_pcap;
}

std::uint32_t
ConfigLoader::SimulationDualChannel ()
{
  return s_val.sim_dual_channel;
}

double
ConfigLoader::SimulationEnergyDeltaTh ()
{
  return s_val.sim_energy_delta_th;
}

double
ConfigLoader::SimulationStateSuppressWin ()
{
  return s_val.sim_state_suppress_win;
}

double
ConfigLoader::SimulationAggregateInterval ()
{
  return s_val.sim_aggregate_interval;
}

std::string
ConfigLoader::SimulationRouteMode ()
{
  return s_val.sim_route_mode;
}

std::string
ConfigLoader::SimulationRouteAdapt ()
{
  return s_val.sim_route_adapt;
}

std::uint32_t
ConfigLoader::SimulationRouteAdaptRuntime ()
{
  return s_val.sim_route_adapt_runtime;
}

double
ConfigLoader::SimulationRouteAdaptRuntimeWindow ()
{
  return s_val.sim_route_adapt_window;
}

double
ConfigLoader::SimulationRouteAdaptRuntimeCooldown ()
{
  return s_val.sim_route_adapt_cooldown;
}


} // namespace hnms
