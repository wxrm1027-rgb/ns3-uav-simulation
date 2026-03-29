#include "event_scheduler.h"

#include "ns3/simulator.h"

namespace hnms
{

EventScheduler&
EventScheduler::Instance ()
{
  static EventScheduler inst;
  return inst;
}

void
EventScheduler::PostAt (ns3::Time t, const std::shared_ptr<SimEvent>& ev)
{
  if (!ev)
    {
      return;
    }
  if (ev->GetMaxRetry () == 0 && m_globalMaxRetry > 0)
    {
      ev->SetMaxRetry (m_globalMaxRetry);
    }
  Entry e{t, static_cast<std::uint8_t> (ev->GetPriority ()), ++m_seq, ev};
  m_q.push (e);
  Arm ();
}

void
EventScheduler::PostIn (ns3::Time dt, const std::shared_ptr<SimEvent>& ev)
{
  ns3::Time at = ns3::Simulator::Now () + dt;
  PostAt (at, ev);
}

void
EventScheduler::CancelAll ()
{
  if (m_timer.IsRunning ())
    {
      ns3::Simulator::Cancel (m_timer);
    }
  m_armed = false;
  while (!m_q.empty ())
    {
      m_q.pop ();
    }
}

void
EventScheduler::Arm ()
{
  if (m_q.empty ())
    {
      return;
    }
  ns3::Time now = ns3::Simulator::Now ();
  ns3::Time next = m_q.top ().when;
  ns3::Time delay = next - now;
  if (delay.IsNegative ())
    {
      delay = ns3::Seconds (0);
    }
  if (m_timer.IsRunning ())
    {
      ns3::Simulator::Cancel (m_timer);
    }
  m_timer = ns3::Simulator::Schedule (delay, &EventScheduler::OnTimer, this);
  m_armed = true;
}

void
EventScheduler::OnTimer ()
{
  m_armed = false;
  if (m_q.empty ())
    {
      return;
    }
  ns3::Time now = ns3::Simulator::Now ();
  Entry cur = m_q.top ();
  if (cur.when > now)
    {
      // 尚未到期：对齐到最早一条的延时
      Arm ();
      return;
    }
  m_q.pop ();

  bool done = false;
  if (cur.ev)
    {
      done = cur.ev->Execute ();
    }
  if (!done && cur.ev && cur.ev->GetRetryCount () < cur.ev->GetMaxRetry ())
    {
      cur.ev->IncRetry ();
      cur.when = now + ns3::MilliSeconds (1);
      m_q.push (cur);
    }
  if (!m_q.empty ())
    {
      Arm ();
    }
}

} // namespace hnms
