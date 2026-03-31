#ifndef HETEROGENEOUS_NMS_HNMP_H
#define HETEROGENEOUS_NMS_HNMP_H

#include <cstdint>

namespace Hnmp
{
struct Header
{
  uint8_t frameType;
  uint8_t qosLevel;
  uint8_t sourceId;
  uint8_t destId;
  uint8_t seq;
  uint8_t payloadLen;
};

enum FrameType : uint8_t
{
  FRAME_REQUEST  = 0x01,
  FRAME_RESPONSE = 0x02,
  FRAME_ALERT    = 0x03,
  FRAME_REPORT   = 0x04,
  FRAME_POLICY   = 0x05,
};

/// implicitCompact3B=true：仅 3 字节头（ft,qos,payloadLen），用于兼容旧版或离线解析；新仿真默认用 false（6 字节全头）
uint32_t EncodeFrame (uint8_t* out,
                      uint32_t outSize,
                      const Header& h,
                      const uint8_t* payload,
                      uint32_t payloadLen,
                      bool implicitCompact3B);

bool DecodeFrame (const uint8_t* in,
                  uint32_t inLen,
                  bool implicitCompact3B,
                  Header* outHeader,
                  const uint8_t** outPayload,
                  uint32_t* outPayloadLen);
} // namespace Hnmp

#endif // HETEROGENEOUS_NMS_HNMP_H

