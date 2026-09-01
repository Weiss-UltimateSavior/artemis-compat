// logger.h — OutputLog-equivalent logging.
//
// Behavior spec (from engine behavior analysis):
//   * level 0/1 -> INFO, 2 -> WARN, 3 -> ERROR; levels > 3 are dropped
//   * Android: __android_log_print(prio, "Artemis", "%s", msg)
//   * host:    stderr with a level prefix
// Level filter: messages below kMinLevel are dropped (keeps the tag flood
// of framework initialization out of logcat by default).
#pragma once
#include <string>

namespace artc {

enum LogLevel {
    kLogDebug = 0,
    kLogInfo  = 1,
    kLogWarn  = 2,
    kLogError = 3,
};

// Messages below this level are dropped. Raise to kLogDebug temporarily when
// tracing framework initialization.
constexpr int kMinLogLevel = kLogInfo;

// Central OutputLog equivalent. Levels > 3 are silently dropped (engine parity).
void Log(int level, const std::string &msg);

// `%s(%d): %s` equivalent for source-tagged trace lines.
void LogTrace(const std::string &file, int line, const std::string &msg);

} // namespace artc
