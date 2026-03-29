#ifndef HNMS_STATE_MANAGER_H
#define HNMS_STATE_MANAGER_H

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hnms
{

/// 发布/订阅全局状态：节点表、链路表、拓扑视图（与 ns-3 业务解耦，供选举/GMC 订阅）
class GlobalStateManager
{
public:
  /// 订阅句柄（取消订阅用）
  using SubscriptionId = std::uint64_t;

  static GlobalStateManager& Instance ();

  /// key 建议使用 "node/<id>/battery" 或 "link/<a>-<b>/snr" 等分层命名
  void UpdateState (const std::string& key, const std::string& valueJson);

  /// 前缀匹配：如 "node/5/" 可收到 node/5/battery、node/5/role
  SubscriptionId Subscribe (const std::string& keyPrefix,
                          std::function<void (const std::string& key, const std::string& value)> cb);
  bool Unsubscribe (SubscriptionId id);

  /// 退网防护：移除所有 node/<id>/* 键并通知订阅者 value 为 null
  void RemoveNode (std::uint32_t nodeId);

  std::string GetState (const std::string& key) const;

private:
  GlobalStateManager () = default;

  mutable std::mutex m_mutex;
  std::map<std::string, std::string> m_kv;

  struct Sub
  {
    std::string prefix;
    std::function<void (const std::string&, const std::string&)> cb;
  };
  std::map<SubscriptionId, Sub> m_subs;
  SubscriptionId m_nextId{1};
};

} // namespace hnms

#endif
