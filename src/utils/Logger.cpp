#include "Logger.h"
#include <chrono>
#include <iomanip>
#include <sstream>

std::mutex Logger::logMutex;

std::string Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARNING: return "WARN";
        case LogLevel::ERR: return "ERROR";
        default: return "UNKNOWN";
    }
}

void Logger::Log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex);
    
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    
    struct tm timeinfo;
    localtime_s(&timeinfo, &in_time_t); // Windows-specific safe localtime

    std::ostringstream ss;
    ss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");

    std::ostream& out = (level == LogLevel::ERR) ? std::cerr : std::cout;
    out << "[" << ss.str() << "] [" << LevelToString(level) << "] " << message << "\n";
}