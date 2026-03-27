#include "hnmp.h"

#include <cstring>

namespace Hnmp
{
uint32_t
EncodeFrame (uint8_t* out,
             uint32_t outSize,
             const Header& h,
             const uint8_t* payload,
             uint32_t payloadLen,
             bool implicitCompact3B)
{
  if (!out || outSize < payloadLen + (implicitCompact3B ? 3u : 6u) || payloadLen > 255u) return 0;
  uint32_t off = 0;
  out[off++] = h.frameType;
  out[off++] = h.qosLevel;
  if (!implicitCompact3B)
    {
      out[off++] = h.sourceId;
      out[off++] = h.destId;
      out[off++] = h.seq;
    }
  out[off++] = static_cast<uint8_t> (payloadLen);
  if (payload && payloadLen > 0) std::memcpy (out + off, payload, payloadLen);
  return off + payloadLen;
}

bool
DecodeFrame (const uint8_t* in,
             uint32_t inLen,
             bool implicitCompact3B,
             Header* outHeader,
             const uint8_t** outPayload,
             uint32_t* outPayloadLen)
{
  if (!in || !outHeader || !outPayload || !outPayloadLen) return false;
  uint32_t minHeader = implicitCompact3B ? 3u : 6u;
  if (inLen < minHeader) return false;
  uint32_t off = 0;
  outHeader->frameType = in[off++];
  outHeader->qosLevel = in[off++];
  if (implicitCompact3B)
    {
      outHeader->sourceId = 0;
      outHeader->destId = 0;
      outHeader->seq = 0;
    }
  else
    {
      outHeader->sourceId = in[off++];
      outHeader->destId = in[off++];
      outHeader->seq = in[off++];
    }
  outHeader->payloadLen = in[off++];
  if (inLen < off + outHeader->payloadLen) return false;
  *outPayload = in + off;
  *outPayloadLen = outHeader->payloadLen;
  return true;
}
} // namespace Hnmp

