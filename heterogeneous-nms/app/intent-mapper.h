#ifndef HNMS_INTENT_MAPPER_H
#define HNMS_INTENT_MAPPER_H

#include "../protocol/tlv.h"
#include <cstdint>
#include <string>

namespace ns3
{

enum PolicyActionType : uint8_t
{
  ACTION_NONE = 0,
  ACTION_TX_POWER = 1,
};

struct PolicyAction
{
  bool valid {false};
  PolicyActionType actionType {ACTION_NONE};
  double value1 {0.0};
  uint8_t intentId {0};
  uint8_t cmdSeq {0};
  uint8_t targetNodeId {0};
  std::string message;
};

class IntentMapper
{
public:
  IntentMapper () = default;
  ~IntentMapper () = default;

  // 将现有 0x14(ExecCmd014) 直接映射为本地动作
  // 当前工程中：
  //   cmd.cmdType  等价于 intentType
  //   cmd.cmdParam 低16位等价于 param1
  PolicyAction Map (const NmsTlv::ExecCmd014& cmd) const;
};

} // namespace ns3

#endif // HNMS_INTENT_MAPPER_H
