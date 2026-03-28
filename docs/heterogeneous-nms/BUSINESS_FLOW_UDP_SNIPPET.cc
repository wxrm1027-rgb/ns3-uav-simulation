/**
 * 业务流 UDP 发包与严格格式日志的示例代码（可直接复制到 heterogeneous-nms-framework.cc 或独立应用）
 *
 * 日志格式（必须严格一致，供 export_visualization_data.py 解析）：
 *   [FLOW_START] flowId=1 priority=2 src=1 dst=22 size=62500 rate=20HZ path=1,2,5,22 type=video
 *   [FLOW_PERF]  flowId=1 delay=15ms throughput=2.5Mbps loss=0.1%
 *
 * FLOW_PERF 由仿真结束后 FlowMonitor 统计输出（见 Run() 末尾），此处仅提供 FLOW_START + UDP 发包逻辑。
 */

#include "ns3/udp-socket-factory.h"
#include "ns3/inet-socket-address.h"
#include "ns3/packet.h"
#include "ns3/simulator.h"
#include "ns3/application.h"

namespace ns3 {

/// 最小化业务流发送应用：按 rateHz 定时发 UDP 包，Start 时打 FLOW_START
class BusinessFlowSender : public Application
{
public:
  static TypeId GetTypeId (void) {
    TypeId tid = TypeId ("ns3::BusinessFlowSender")
      .SetParent<Application> ()
      .AddConstructor<BusinessFlowSender> ();
    return tid;
  }
  BusinessFlowSender () : m_socket (0), m_sent (0) {}

  void SetRemote (Ipv4Address ip, uint16_t port) { m_peerAddr = ip; m_peerPort = port; }
  void SetFlowId (uint32_t id) { m_flowId = id; }
  void SetPriority (uint32_t p) { m_priority = p; }
  void SetSrcDst (uint32_t src, uint32_t dst) { m_srcId = src; m_dstId = dst; }
  void SetSize (uint32_t size) { m_pktSize = size; }
  void SetRateHz (double hz) { m_rateHz = hz; }
  void SetType (const std::string& type) { m_flowType = type; }
  void SetPath (const std::vector<uint32_t>& path) { m_path = path; }

  void SetLogCallback (std::function<void(const std::string&)> cb) { m_logCb = cb; }

protected:
  void DoDispose () override { m_socket = 0; Application::DoDispose (); }

  void StartApplication () override
  {
    m_socket = Socket::CreateSocket (GetNode (), UdpSocketFactory::GetTypeId ());
    if (m_socket->Connect (InetSocketAddress (m_peerAddr, m_peerPort)) != 0)
      return;
    // 严格格式 FLOW_START（与 Python 解析一致）
    std::ostringstream oss;
    oss << "flowId=" << m_flowId << " priority=" << m_priority << " src=" << m_srcId
        << " dst=" << m_dstId << " size=" << m_pktSize << " rate=" << (int)m_rateHz << "HZ"
        << " type=" << m_flowType;
    if (!m_path.empty ()) {
      oss << " path=";
      for (size_t i = 0; i < m_path.size (); ++i) oss << (i ? "," : "") << m_path[i];
    }
    if (m_logCb) m_logCb (oss.str ());
    // 定时发包
    if (m_rateHz > 0)
      Simulator::Schedule (Seconds (1.0 / m_rateHz), &BusinessFlowSender::SendPacket, this);
  }

  void StopApplication () override { m_socket->Close (); }

  void SendPacket ()
  {
    if (!m_socket || m_socket->GetErrno () != Socket::ERROR_NOTERROR) return;
    Ptr<Packet> pkt = Create<Packet> (m_pktSize);
    m_socket->Send (pkt);
    m_sent++;
    if (m_rateHz > 0)
      Simulator::Schedule (Seconds (1.0 / m_rateHz), &BusinessFlowSender::SendPacket, this);
  }

private:
  Ptr<Socket> m_socket;
  Ipv4Address m_peerAddr;
  uint16_t m_peerPort;
  uint32_t m_flowId, m_priority, m_srcId, m_dstId, m_pktSize;
  double m_rateHz;
  std::string m_flowType;
  std::vector<uint32_t> m_path;
  uint64_t m_sent;
  std::function<void(const std::string&)> m_logCb;
};

} // namespace ns3

/**
 * 在 Framework 中安装示例（伪代码，需在 InstallApplications 中调用）：
 *
 *   Ptr<BusinessFlowSender> app = CreateObject<BusinessFlowSender> ();
 *   app->SetRemote (m_ifAdhoc.GetAddress (dstIdx), 9000);
 *   app->SetFlowId (1);
 *   app->SetPriority (2);
 *   app->SetSrcDst (flow1SrcId, flow1DstId);
 *   app->SetSize (62500);
 *   app->SetRateHz (20);
 *   app->SetType ("video");
 *   std::vector<uint32_t> path = GetOlsrPath (flow1SrcId, flow1DstId);
 *   app->SetPath (path);
 *   app->SetLogCallback ([this] (const std::string& details) {
 *     NmsLog ("INFO", 0, "FLOW_START", details);
 *   });
 *   m_adhocNodes.Get (flow1SrcIdx)->AddApplication (app);
 *   app->SetStartTime (Seconds (5.0));
 *   app->SetStopTime (simTime);
 *
 * FLOW_PERF 在 Run() 末尾由 FlowMonitor 统计后打印，格式示例：
 *   NmsLog ("INFO", 0, "FLOW_PERF", "flowId=1 delay=15ms throughput=2.5Mbps loss=0.1%");
 */
