#pragma once
#include <string>
#include <mutex>
#include <iostream>

enum class LogLevel { INFO, WARNING, ERR }; // ERR instead of ERROR to avoid Windows macro conflicts

/// Minimal thread-safe logger.
///
/// Writes "[timestamp] [LEVEL] message" lines to stdout (stderr for ERR),
/// serialized by a global mutex so concurrent worker output doesn't
/// interleave. Use the LOG_* macros rather than calling Log directly.
class Logger {
public:
    static void Log(LogLevel level, const std::string& message);

private:
    static std::mutex logMutex;
    static std::string LevelToString(LogLevel level);
};

#define LOG_INFO(msg) Logger::Log(LogLevel::INFO, msg)
#define LOG_WARN(msg) Logger::Log(LogLevel::WARNING, msg)
#define LOG_ERR(msg) Logger::Log(LogLevel::ERR, msg)
