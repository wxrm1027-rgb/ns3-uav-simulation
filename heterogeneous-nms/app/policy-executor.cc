#include "policy-executor.h"
#include "../trace/policy-state-tracer.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/net-device.h"
#include "ns3/node.h"
#include "ns3/wifi-net-device.h"
#include "ns3/wifi-phy.h"

#include <cmath>
#include <limits>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE ("PolicyExecutor");

PolicyExecutor::PolicyExecutor ()
  : m_node (nullptr),
    m_subnetLabel ("unknown"),
    m_hasCachedTxPowerDbm (false),
    m_cachedTxPowerDbm (0.0)
{
}

void
PolicyExecutor::SetNode (Ptr<Node> node)
{
  m_node = node;
}

void
PolicyExecutor::SetSubnetLabel (const std::string& subnetLabel)
{
  m_subnetLabel = subnetLabel;
}

ExecStatus
PolicyExecutor::Execute (const PolicyAction& action)
{
  if (!action.valid)
    {
      ExecStatus st;
      st.success = false;
      st.errorCode = EXEC_ERR_UNSUPPORTED_ACTION;
      st.message = "invalid action";
      NS_LOG_WARN ("[POLICY_EXEC] node=-1 action=INVALID old=0 new=0 success=0 err="
                   << static_cast<uint32_t> (st.errorCode)
                   << " msg=" << st.message);
      return st;
    }

  switch (action.actionType)
    {
    case ACTION_TX_POWER:
      return ExecuteTxPower (action.value1);

    case ACTION_NONE:
    default:
      {
        ExecStatus st;
        st.success = false;
        st.errorCode = EXEC_ERR_UNSUPPORTED_ACTION;
        st.message = "unsupported action type";
        NS_LOG_WARN ("[POLICY_EXEC] node="
                     << (m_node ? static_cast<int64_t> (m_node->GetId ()) : -1)
                     << " action=NONE old=0 new=0 success=0 err="
                     << static_cast<uint32_t> (st.errorCode)
                     << " msg=" << st.message);
        return st;
      }
    }
}

ExecStatus
PolicyExecutor::ExecuteTxPower (double txPowerDbm)
{
  ExecStatus st;
  st.newValue = txPowerDbm;
  st.oldValue = m_hasCachedTxPowerDbm
                  ? m_cachedTxPowerDbm
                  : std::numeric_limits<double>::quiet_NaN ();

  hnms::PolicyStateTraceRecord beforeRecord;
  beforeRecord.time = ns3::Simulator::Now ().GetSeconds ();
  beforeRecord.nodeId = m_node ? m_node->GetId () : 0;
  beforeRecord.subnet = m_subnetLabel;
  beforeRecord.intentType = 1;
  beforeRecord.stage = "EXEC_BEFORE";
  beforeRecord.oldTxPower = st.oldValue;
  beforeRecord.newTxPower = txPowerDbm;
  beforeRecord.hasWifi = false;
  beforeRecord.selected = true;
  beforeRecord.success = false;
  beforeRecord.errorCode = EXEC_ERR_NONE;
  beforeRecord.reason = "before_execute_tx_power";

  if (!m_node)
    {
      st.success = false;
      st.errorCode = EXEC_ERR_INVALID_NODE;
      st.message = "node is null";
      beforeRecord.errorCode = st.errorCode;
      beforeRecord.reason = st.message;
      hnms::PolicyStateTracer::Log (beforeRecord);

      hnms::PolicyStateTraceRecord afterRecord = beforeRecord;
      afterRecord.time = ns3::Simulator::Now ().GetSeconds ();
      afterRecord.stage = "EXEC_AFTER";
      afterRecord.success = false;
      afterRecord.errorCode = st.errorCode;
      afterRecord.reason = st.message;
      hnms::PolicyStateTracer::Log (afterRecord);

      NS_LOG_WARN ("[POLICY_EXEC] node=-1 action=TX_POWER old="
                   << st.oldValue
                   << " new=" << st.newValue
                   << " success=0 err=" << static_cast<uint32_t> (st.errorCode)
                   << " msg=" << st.message);
      return st;
    }

  if (!std::isfinite (txPowerDbm))
    {
      st.success = false;
      st.errorCode = EXEC_ERR_INVALID_PARAM;
      st.message = "txPowerDbm is not finite";
      beforeRecord.errorCode = st.errorCode;
      beforeRecord.reason = st.message;
      hnms::PolicyStateTracer::Log (beforeRecord);

      hnms::PolicyStateTraceRecord afterRecord = beforeRecord;
      afterRecord.time = ns3::Simulator::Now ().GetSeconds ();
      afterRecord.stage = "EXEC_AFTER";
      afterRecord.success = false;
      afterRecord.errorCode = st.errorCode;
      afterRecord.reason = st.message;
      hnms::PolicyStateTracer::Log (afterRecord);

      NS_LOG_WARN ("[POLICY_EXEC] node=" << m_node->GetId ()
                   << " action=TX_POWER old=" << st.oldValue
                   << " new=" << st.newValue
                   << " success=0 err=" << static_cast<uint32_t> (st.errorCode)
                   << " msg=" << st.message);
      return st;
    }

  for (uint32_t i = 0; i < m_node->GetNDevices (); ++i)
    {
      Ptr<NetDevice> dev = m_node->GetDevice (i);
      Ptr<WifiNetDevice> wifiDev = DynamicCast<WifiNetDevice> (dev);
      if (!wifiDev)
        {
          continue;
        }

      beforeRecord.hasWifi = true;
      hnms::PolicyStateTracer::Log (beforeRecord);

      Ptr<WifiPhy> phy = wifiDev->GetPhy ();
      if (!phy)
        {
          st.success = false;
          st.errorCode = EXEC_ERR_WIFI_PHY_NOT_FOUND;
          st.message = "WifiNetDevice found but WifiPhy is null";

          hnms::PolicyStateTraceRecord afterRecord = beforeRecord;
          afterRecord.time = ns3::Simulator::Now ().GetSeconds ();
          afterRecord.stage = "EXEC_AFTER";
          afterRecord.success = false;
          afterRecord.errorCode = st.errorCode;
          afterRecord.reason = st.message;
          hnms::PolicyStateTracer::Log (afterRecord);

          NS_LOG_WARN ("[POLICY_EXEC] node=" << m_node->GetId ()
                       << " action=TX_POWER old=" << st.oldValue
                       << " new=" << st.newValue
                       << " success=0 err=" << static_cast<uint32_t> (st.errorCode)
                       << " msg=" << st.message);
          return st;
        }

      phy->SetTxPowerStart (txPowerDbm);
      phy->SetTxPowerEnd (txPowerDbm);

      m_cachedTxPowerDbm = txPowerDbm;
      m_hasCachedTxPowerDbm = true;
      hnms::PolicyStateTracer::UpdateMirroredTxPower (m_node->GetId (), txPowerDbm);

      st.success = true;
      st.errorCode = EXEC_ERR_NONE;

      std::ostringstream oss;
      oss << "wifi phy tx power updated on devIndex=" << i;
      st.message = oss.str ();

      hnms::PolicyStateTraceRecord afterRecord = beforeRecord;
      afterRecord.time = ns3::Simulator::Now ().GetSeconds ();
      afterRecord.stage = "EXEC_AFTER";
      afterRecord.newTxPower = txPowerDbm;
      afterRecord.success = true;
      afterRecord.errorCode = st.errorCode;
      afterRecord.reason = st.message;
      hnms::PolicyStateTracer::Log (afterRecord);

      NS_LOG_INFO ("[POLICY_EXEC] node=" << m_node->GetId ()
                   << " action=TX_POWER old=" << st.oldValue
                   << " new=" << st.newValue
                   << " success=1 err=" << static_cast<uint32_t> (st.errorCode)
                   << " msg=" << st.message);
      return st;
    }

  beforeRecord.hasWifi = false;
  hnms::PolicyStateTracer::Log (beforeRecord);

  st.success = false;
  st.errorCode = EXEC_ERR_WIFI_DEVICE_NOT_FOUND;
  st.message = "no WifiNetDevice found on node";

  hnms::PolicyStateTraceRecord afterRecord = beforeRecord;
  afterRecord.time = ns3::Simulator::Now ().GetSeconds ();
  afterRecord.stage = "EXEC_AFTER";
  afterRecord.success = false;
  afterRecord.errorCode = st.errorCode;
  afterRecord.reason = st.message;
  hnms::PolicyStateTracer::Log (afterRecord);

  NS_LOG_WARN ("[POLICY_EXEC] node=" << m_node->GetId ()
               << " action=TX_POWER old=" << st.oldValue
               << " new=" << st.newValue
               << " success=0 err=" << static_cast<uint32_t> (st.errorCode)
               << " msg=" << st.message);
  return st;
}

} // namespace ns3
