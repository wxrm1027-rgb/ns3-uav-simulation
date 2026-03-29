#ifndef HNMS_STRUCTURED_LOG_H
#define HNMS_STRUCTURED_LOG_H

#include <cstdint>
#include <string>

namespace hnms
{

/// 日志级别（与 NmsLog 字符串兼容）
enum class LogLevel
{
  DEBUG,
  INFO,
  WARN,
  ERROR
};

/// 结构化 JSON 行日志 + TraceId；可与原有 NmsLog 并行启用
class StructuredLog
{
public:
  static void SetJsonEnabled (bool on) { s_json = on; }
  static bool IsJsonEnabled () { return s_json; }

  static std::string NewTraceId ();
  static void Write (LogLevel level, std::uint32_t nodeId, const std::string& eventType,
                     const std::string& traceId, const std::string& msg);

  static const char* LevelStr (LogLevel l);

private:
  static bool s_json;
};

} // namespace hnms

#endif
