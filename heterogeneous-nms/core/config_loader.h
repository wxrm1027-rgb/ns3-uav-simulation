#ifndef HNMS_CONFIG_LOADER_H
#define HNMS_CONFIG_LOADER_H

#include <cstdint>
#include <string>

namespace hnms
{

/// 从 config.json 加载仿真参数（缺省值与文件合并）。解析为轻量实现，不依赖第三方 JSON 库。
class ConfigLoader
{
public:
  /// 若 path 为空或打开失败，仅使用内置默认
  static void LoadFromFile (const std::string& path);

  static double SpnElectionWaitTime ();
  static double SpnHelloInterval ();
  static double SpnSuppressWindow ();
  /// 与框架默认 m_spnHeartbeatMissThreshold=3 对齐
  static std::uint32_t SpnHeartbeatMissThreshold ();

  static double NodeBatteryLow ();
  static double NodeSpeedMax ();

  static double LinkSnrConnect ();
  static double LinkSnrDisconnect ();

  static double QosMaxLatencyMs ();
  static double QosMinThroughputMbps ();

  static double SimulationTotalTime ();
  static std::uint32_t SimulationNumNodes ();

  static std::string LastPath ();

private:
  struct Defaults
  {
    double spn_election_wait{2.0};
    double spn_hello{0.5};
    double spn_suppress{2.0};
    std::uint32_t spn_heartbeat_miss{3};
    double node_batt_low{0.2};
    double node_speed_max{50.0};
    double link_snr_conn{10.0};
    double link_snr_disconn{5.0};
    double qos_lat{100.0};
    double qos_tp{10.0};
    double sim_time{100.0};
    std::uint32_t sim_nodes{30};
  };

  static Defaults s_val;
  static std::string s_path;
};

} // namespace hnms

#endif
