#include "log/logger.h"

#include <cstdio>
#include <mutex>

#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__OHOS__)
#include <hilog/log.h>
#endif

namespace artc {

namespace {
const char *kTag = "Artemis";
#if defined(__OHOS__)
// Distinct hilog domain from the krkr2 core (0x0206) so the two engines can
// be filtered apart: `hilog | grep Artemis`.
constexpr unsigned int kOhosLogDomain = 0x0207;
#endif
[[maybe_unused]] const char *LevelName(int level) {
    switch (level) {
    case kLogDebug: return "DEBUG";
    case kLogInfo:  return "INFO";
    case kLogWarn:  return "WARN";
    case kLogError: return "ERROR";
    default:        return "L?";
    }
}

std::mutex g_sink_mutex;
LogSink g_sink;
} // namespace

void SetLogSink(LogSink sink) {
    std::lock_guard<std::mutex> lk(g_sink_mutex);
    g_sink = std::move(sink);
}

void Log(int level, const std::string &msg) {
    if (level < kMinLogLevel || level > kLogError) return; // filtered + engine parity
#if defined(__ANDROID__)
    int prio = (level <= kLogInfo) ? ANDROID_LOG_INFO
             : (level == kLogWarn) ? ANDROID_LOG_WARN
                                   : ANDROID_LOG_ERROR;
    __android_log_print(prio, kTag, "%s", msg.c_str());
#elif defined(__OHOS__)
    // `::LogLevel` is hilog's enum; artc::LogLevel shadows it in this scope.
    ::LogLevel prio = (level <= kLogInfo) ? LOG_INFO
                    : (level == kLogWarn) ? LOG_WARN
                                          : LOG_ERROR;
    OH_LOG_Print(LOG_APP, prio, kOhosLogDomain, kTag, "%{public}s", msg.c_str());
#else
    std::fprintf(stderr, "[%s] [%s] %s\n", kTag, LevelName(level), msg.c_str());
#endif
    LogSink sink;
    {
        std::lock_guard<std::mutex> lk(g_sink_mutex);
        sink = g_sink;
    }
    if (sink) sink(level, msg);
}

void LogTrace(const std::string &file, int line, const std::string &msg) {
    Log(kLogDebug, file + "(" + std::to_string(line) + "): " + msg);
}

} // namespace artc
