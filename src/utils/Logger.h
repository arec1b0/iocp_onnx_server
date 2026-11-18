#pragma once
#include <string_view>

enum class LogLevel { INFO, WARNING, ERR }; // ERR instead of ERROR to avoid Windows macro conflicts

/// Thread-safe, best-effort output. Redirected Windows pipes use non-waiting writes.
class Logger {
public:
    static void Log(LogLevel level, std::string_view message);
    static void WriteJsonLine(std::string_view json);
    /// Publishes the final snapshot after network shutdown and reports any lost records.
    /// Returns false if a delivery failure could not be reported. Further calls are idempotent.
    static bool Finish(std::string_view finalJson = {});
};

#define LOG_INFO(msg) Logger::Log(LogLevel::INFO, msg)
#define LOG_WARN(msg) Logger::Log(LogLevel::WARNING, msg)
#define LOG_ERR(msg) Logger::Log(LogLevel::ERR, msg)
