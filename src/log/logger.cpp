#include "log/logger.h"

#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace artc {

namespace {
const char *kTag = "Artemis";
const char *LevelName(int level) {
    switch (level) {
    case kLogDebug: return "DEBUG";
    case kLogInfo:  return "INFO";
    case kLogWarn:  return "WARN";
    case kLogError: return "ERROR";
    default:        return "L?";
    }
}
} // namespace

void Log(int level, const std::string &msg) {
    if (level < kMinLogLevel || level > kLogError) return; // filtered + engine parity
#if defined(__ANDROID__)
    int prio = (level <= kLogInfo) ? ANDROID_LOG_INFO
             : (level == kLogWarn) ? ANDROID_LOG_WARN
                                   : ANDROID_LOG_ERROR;
    __android_log_print(prio, kTag, "%s", msg.c_str());
#else
    std::fprintf(stderr, "[%s] %s\n", LevelName(level), msg.c_str());
#endif
}

void LogTrace(const std::string &file, int line, const std::string &msg) {
    Log(kLogDebug, file + "(" + std::to_string(line) + "): " + msg);
}

} // namespace artc
