#ifndef HNMS_EVENT_SCHEDULER_H
#define HNMS_EVENT_SCHEDULER_H

#include "sim_event.h"

#include "ns3/nstime.h"
#include "ns3/event-id.h"

#include <memory>
#include <queue>
#include <vector>

namespace hnms
{

/// 基于 ns-3 Simulator 的优先级事件调度：同仿真时刻优先执行 Critical > High > Normal > Low
/// 支持 max_retry、取消与下一事件链式 Schedule
class EventScheduler
{
public:
  static EventScheduler& Instance ();

  void SetGlobalMaxRetry (std::uint32_t n) { m_globalMaxRetry = n; }
  std::uint32_t GetGlobalMaxRetry () const { return m_globalMaxRetry; }

  /// 在仿真时刻 t 投递事件（相对当前仿真时间应 >= 0，由调用方保证）
  void PostAt (ns3::Time t, const std::shared_ptr<SimEvent>& ev);

  /// 便捷：延迟 dt 投递
  void PostIn (ns3::Time dt, const std::shared_ptr<SimEvent>& ev);

  void CancelAll ();

private:
  EventScheduler () = default;

  struct Entry
  {
    ns3::Time when;
    /// 越大越先执行（与 SimEventPriority 数值一致）
    std::uint8_t pri;
    std::uint64_t seq;
    std::shared_ptr<SimEvent> ev;
  };

  struct Cmp
  {
    bool operator() (const Entry& a, const Entry& b) const
    {
      if (a.when != b.when)
        return a.when > b.when; // min-heap on time
      if (a.pri != b.pri)
        return a.pri < b.pri; // larger pri first
      return a.seq > b.seq;
    }
  };

  void Arm ();
  void OnTimer ();

  std::priority_queue<Entry, std::vector<Entry>, Cmp> m_q;
  std::uint64_t m_seq{0};
  ns3::EventId m_timer;
  bool m_armed{false};
  std::uint32_t m_globalMaxRetry{5};
};

} // namespace hnms

#endif
