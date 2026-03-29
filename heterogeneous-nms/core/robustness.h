#ifndef HNMS_ROBUSTNESS_H
#define HNMS_ROBUSTNESS_H

#include <cstdint>

namespace hnms
{

/// 防护：节点 ID、邻居索引等合法性检查（边界条件集中入口）
inline bool
IsValidNodeId (std::uint32_t id, std::uint32_t maxExclusive)
{
  return id < maxExclusive;
}

/// 子网分裂/合并、SPN 切换的完整协议应在应用层实现；此处仅提供优先级比较辅助（电量高优先，同电量 ID 小优先）
inline bool
SpnPriorityCompare (double energyA, std::uint32_t idA, double energyB, std::uint32_t idB)
{
  if (energyA != energyB)
    {
      return energyA > energyB;
    }
  return idA < idB;
}

} // namespace hnms

#endif
