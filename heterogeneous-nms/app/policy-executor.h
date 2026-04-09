#ifndef HNMS_POLICY_EXECUTOR_H
#define HNMS_POLICY_EXECUTOR_H

#include "intent-mapper.h"
#include "ns3/node.h"
#include <string>

namespace ns3
{

enum ExecErrorCode : uint8_t
{
  EXEC_ERR_NONE = 0,
  EXEC_ERR_INVALID_NODE = 1,
  EXEC_ERR_UNSUPPORTED_ACTION = 2,
  EXEC_ERR_INVALID_PARAM = 3,
  EXEC_ERR_WIFI_DEVICE_NOT_FOUND = 4,
  EXEC_ERR_WIFI_PHY_NOT_FOUND = 5,
  EXEC_ERR_INTERNAL = 255,
};

struct ExecStatus
{
  bool success {false};
  uint8_t errorCode {EXEC_ERR_INTERNAL};
  double oldValue {0.0};
  double newValue {0.0};
  std::string message;
};

class PolicyExecutor
{
public:
  PolicyExecutor ();
  ~PolicyExecutor () = default;

  void SetNode (Ptr<Node> node);
  void SetSubnetLabel (const std::string& subnetLabel);
  ExecStatus Execute (const PolicyAction& action);
  ExecStatus ExecuteTxPower (double txPowerDbm);

private:
  Ptr<Node> m_node;
  std::string m_subnetLabel;

  // 本地缓存最近一次设置的发射功率，便于输出 old/new 值。
  bool m_hasCachedTxPowerDbm;
  double m_cachedTxPowerDbm;
};

} // namespace ns3

#endif // HNMS_POLICY_EXECUTOR_H
