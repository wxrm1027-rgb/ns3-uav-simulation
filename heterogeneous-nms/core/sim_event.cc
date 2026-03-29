#include "sim_event.h"

#include "state_manager.h"
#include "structured_log.h"

#include <sstream>

namespace hnms
{

static void
NotifyState (std::uint32_t nodeId, const std::string& field, double v)
{
  std::ostringstream key, val;
  key << "node/" << nodeId << "/" << field;
  val << v;
  GlobalStateManager::Instance ().UpdateState (key.str (), val.str ());
}

bool
NodeStateEvent::Execute ()
{
  NotifyState (m_nodeId, m_field, m_value);
  StructuredLog::Write (LogLevel::INFO, m_nodeId, "NODE_STATE", StructuredLog::NewTraceId (),
                        "field=" + m_field + " value=" + std::to_string (m_value));
  return true;
}

bool
LinkEvent::Execute ()
{
  std::ostringstream key, val;
  key << "link/" << m_a << "-" << m_b << "/up";
  val << (m_up ? "true" : "false");
  GlobalStateManager::Instance ().UpdateState (key.str (), val.str ());
  std::ostringstream msg;
  msg << "peer=" << m_b << " up=" << m_up << " q=" << m_quality;
  StructuredLog::Write (LogLevel::INFO, m_a, "LINK", StructuredLog::NewTraceId (), msg.str ());
  return true;
}

bool
SpnElectionEvent::Execute ()
{
  std::string tid = StructuredLog::NewTraceId ();
  StructuredLog::Write (LogLevel::INFO, 0, "SPN_ELECTION_START", tid,
                        "subnet=" + std::to_string (m_subnetKey) + " reason=" + m_reason);
  return true;
}

bool
GmcPolicyEvent::Execute ()
{
  std::string tid = StructuredLog::NewTraceId ();
  StructuredLog::Write (LogLevel::INFO, m_target, "GMC_POLICY", tid, "tag=" + m_policyTag);
  return true;
}

bool
TopologyReportEvent::Execute ()
{
  StructuredLog::Write (LogLevel::DEBUG, m_reporter, "TOPOLOGY_REPORT", StructuredLog::NewTraceId (),
                        "heartbeat");
  return true;
}

bool
HelloTimeoutEvent::Execute ()
{
  StructuredLog::Write (LogLevel::WARN, m_node, "HELLO_TIMEOUT", StructuredLog::NewTraceId (),
                        "peer=" + std::to_string (m_peer));
  return true;
}

bool
RouteChangeEvent::Execute ()
{
  StructuredLog::Write (LogLevel::INFO, m_node, "ROUTE_CHANGE", StructuredLog::NewTraceId (),
                        "dst=" + m_dstKey);
  return true;
}

bool
PolicyAckEvent::Execute ()
{
  StructuredLog::Write (LogLevel::INFO, m_node, "POLICY_ACK", StructuredLog::NewTraceId (),
                        "seq=" + std::to_string (m_seq));
  return true;
}

} // namespace hnms
