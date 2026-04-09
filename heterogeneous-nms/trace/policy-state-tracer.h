#ifndef HNMS_POLICY_STATE_TRACER_H
#define HNMS_POLICY_STATE_TRACER_H

#include "ns3/object.h"
#include <cstdint>
#include <map>
#include <string>

namespace hnms
{

struct PolicyStateTraceRecord
{
  double time {0.0};
  uint32_t nodeId {0};
  std::string subnet;
  uint8_t intentType {0};
  std::string stage;
  double oldTxPower {0.0};
  double newTxPower {0.0};
  bool hasWifi {false};
  bool selected {false};
  bool success {false};
  uint32_t errorCode {0};
  std::string reason;
};

class PolicyStateTracer
{
public:
  static void Init (const std::string& fileName);
  static void Log (const PolicyStateTraceRecord& record);
  static void Close ();
  static bool IsInitialized ();

  // 当当前工程无法可靠读取 WifiPhy 实时 TxPower 时，
  // 使用镜像状态变量记录成功执行后的最新值。
  static void UpdateMirroredTxPower (uint32_t nodeId, double txPowerDbm);
  static double GetMirroredTxPower (uint32_t nodeId, bool* hasValue = nullptr);

private:
  PolicyStateTracer () = delete;
};

} // namespace hnms

#endif // HNMS_POLICY_STATE_TRACER_H
