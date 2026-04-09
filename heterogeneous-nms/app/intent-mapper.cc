#include "intent-mapper.h"
#include "ns3/log.h"
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE ("IntentMapper");

PolicyAction
IntentMapper::Map (const NmsTlv::ExecCmd014& cmd) const
{
  PolicyAction action;
  action.intentId = cmd.intentId;
  action.cmdSeq = cmd.cmdSeq;
  action.targetNodeId = cmd.targetNodeId;

  // 当前工程中：
  //   cmdType <- intentType
  //   cmdParam <- (validTime << 16) | param1
  // 因此这里取低16位作为 param1，并映射为 TxPower(dBm)
  const uint8_t intentType = cmd.cmdType;
  const uint16_t param1 = static_cast<uint16_t> (cmd.cmdParam & 0xffffu);

  if (intentType == 1)
    {
      action.valid = true;
      action.actionType = ACTION_TX_POWER;
      action.value1 = static_cast<double> (param1);

      std::ostringstream oss;
      oss << "mapped cmdType=" << static_cast<uint32_t> (intentType)
          << " to ACTION_TX_POWER, txPowerDbm=" << action.value1
          << ", intentId=" << static_cast<uint32_t> (action.intentId)
          << ", cmdSeq=" << static_cast<uint32_t> (action.cmdSeq);
      action.message = oss.str ();
      NS_LOG_INFO ("[POLICY_EXEC] " << action.message);
      return action;
    }

  {
    std::ostringstream oss;
    oss << "unsupported cmdType=" << static_cast<uint32_t> (intentType)
        << ", intentId=" << static_cast<uint32_t> (action.intentId)
        << ", cmdSeq=" << static_cast<uint32_t> (action.cmdSeq);
    action.message = oss.str ();
    NS_LOG_WARN ("[POLICY_EXEC] " << action.message);
  }

  return action;
}

} // namespace ns3
