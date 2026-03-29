#ifndef HNMS_SIM_EVENT_H
#define HNMS_SIM_EVENT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace hnms
{

/// 与调度器一致：数值越大越优先（故障/退网最先执行）
enum class SimEventPriority : std::uint8_t
{
  Low = 0,     ///< 常规上报、周期性拓扑快照
  Normal = 1,  ///< 拓扑上报、策略 ACK
  High = 2,    ///< SPN 选举、Hello 超时
  Critical = 3 ///< 节点退网、链路失效、GMC 紧急策略
};

/// 仿真事件基类：由 EventScheduler 在仿真时刻回调 Execute
class SimEvent
{
public:
  virtual ~SimEvent () = default;

  SimEventPriority GetPriority () const { return m_priority; }
  void SetPriority (SimEventPriority p) { m_priority = p; }

  const std::string& GetTraceId () const { return m_traceId; }
  void SetTraceId (const std::string& id) { m_traceId = id; }

  std::uint32_t GetMaxRetry () const { return m_maxRetry; }
  void SetMaxRetry (std::uint32_t n) { m_maxRetry = n; }

  std::uint32_t GetRetryCount () const { return m_retryCount; }
  void IncRetry () { ++m_retryCount; }

  virtual std::string GetTypeName () const = 0;
  /// 返回 true 表示已处理完毕；false 且未达 max_retry 时调度器可重排队
  virtual bool Execute () = 0;

protected:
  SimEventPriority m_priority{SimEventPriority::Normal};
  std::string m_traceId;
  std::uint32_t m_maxRetry{0}; ///< 0 表示使用 EventScheduler 全局上限
  std::uint32_t m_retryCount{0};
};

// --- 派生事件（可按协议扩展） ---

class NodeStateEvent : public SimEvent
{
public:
  explicit NodeStateEvent (std::uint32_t nodeId, const std::string& field, double value)
      : m_nodeId (nodeId), m_field (field), m_value (value)
  {
    m_priority = SimEventPriority::Normal;
  }
  std::string GetTypeName () const override { return "NodeStateEvent"; }
  bool Execute () override;
  std::uint32_t m_nodeId;
  std::string m_field;
  double m_value;
};

class LinkEvent : public SimEvent
{
public:
  LinkEvent (std::uint32_t a, std::uint32_t b, bool up, double quality01)
      : m_a (a), m_b (b), m_up (up), m_quality (quality01)
  {
    m_priority = SimEventPriority::High;
  }
  std::string GetTypeName () const override { return "LinkEvent"; }
  bool Execute () override;
  std::uint32_t m_a, m_b;
  bool m_up;
  double m_quality;
};

class SpnElectionEvent : public SimEvent
{
public:
  explicit SpnElectionEvent (std::uint32_t subnetKey, const std::string& reason)
      : m_subnetKey (subnetKey), m_reason (reason)
  {
    m_priority = SimEventPriority::High;
  }
  std::string GetTypeName () const override { return "SpnElectionEvent"; }
  bool Execute () override;
  std::uint32_t m_subnetKey;
  std::string m_reason;
};

class GmcPolicyEvent : public SimEvent
{
public:
  GmcPolicyEvent (std::uint32_t targetNode, const std::string& policyTag)
      : m_target (targetNode), m_policyTag (policyTag)
  {
    m_priority = SimEventPriority::Critical;
  }
  std::string GetTypeName () const override { return "GmcPolicyEvent"; }
  bool Execute () override;
  std::uint32_t m_target;
  std::string m_policyTag;
};

class TopologyReportEvent : public SimEvent
{
public:
  explicit TopologyReportEvent (std::uint32_t reporterId) : m_reporter (reporterId)
  {
    m_priority = SimEventPriority::Low;
  }
  std::string GetTypeName () const override { return "TopologyReportEvent"; }
  bool Execute () override;
  std::uint32_t m_reporter;
};

/// 补充：Hello 超时、路由变化、策略确认（与现有 TLV/Hello 对齐）
class HelloTimeoutEvent : public SimEvent
{
public:
  HelloTimeoutEvent (std::uint32_t nodeId, std::uint32_t peerId)
      : m_node (nodeId), m_peer (peerId)
  {
    m_priority = SimEventPriority::High;
  }
  std::string GetTypeName () const override { return "HelloTimeoutEvent"; }
  bool Execute () override;
  std::uint32_t m_node, m_peer;
};

class RouteChangeEvent : public SimEvent
{
public:
  RouteChangeEvent (std::uint32_t nodeId, const std::string& dstKey) : m_node (nodeId), m_dstKey (dstKey)
  {
    m_priority = SimEventPriority::Normal;
  }
  std::string GetTypeName () const override { return "RouteChangeEvent"; }
  bool Execute () override;
  std::uint32_t m_node;
  std::string m_dstKey;
};

class PolicyAckEvent : public SimEvent
{
public:
  PolicyAckEvent (std::uint32_t nodeId, std::uint32_t seq) : m_node (nodeId), m_seq (seq)
  {
    m_priority = SimEventPriority::Normal;
  }
  std::string GetTypeName () const override { return "PolicyAckEvent"; }
  bool Execute () override;
  std::uint32_t m_node, m_seq;
};

} // namespace hnms

#endif // HNMS_SIM_EVENT_H
