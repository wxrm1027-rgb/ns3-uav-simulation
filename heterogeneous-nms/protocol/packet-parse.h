#ifndef HETEROGENEOUS_NMS_PACKET_PARSE_H
#define HETEROGENEOUS_NMS_PACKET_PARSE_H

#include "ns3/ipv4-address.h"
#include <cstdint>
#include <string>

namespace NmsPacketParse
{
void SetEnable (bool enable);
bool IsEnabled ();
std::string ToHex (const uint8_t* data, uint32_t len, uint32_t maxBytes = 64);
std::string ParseTlvValue (const uint8_t* data, uint32_t len);
void LogDataPacket (uint32_t nodeId, const char* direction,
                    ns3::Ipv4Address srcAddr, ns3::Ipv4Address dstAddr, uint16_t srcPort, uint16_t dstPort,
                    const uint8_t* data, uint32_t len);
void LogHelloPacket (uint32_t nodeId, const char* direction,
                     ns3::Ipv4Address fromAddr, uint16_t fromPort,
                     const uint8_t* data, uint32_t len);
void LogPolicyPacket (uint32_t nodeId, const char* direction,
                      ns3::Ipv4Address fromAddr, uint16_t fromPort,
                      const uint8_t* data, uint32_t len);
} // namespace NmsPacketParse

#endif // HETEROGENEOUS_NMS_PACKET_PARSE_H

