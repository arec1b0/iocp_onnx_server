#include "Metrics.h"
#include "Logger.h"
#include <chrono>
#include <cmath>

Metrics& Metrics::GetInstance() {
    static Metrics instance;
    return instance;
}

Metrics::Metrics() {
    logFile_.open("metrics.jsonl", std::ios::app);
    if (!logFile_.is_open()) {
        LOG_ERR("Failed to open metrics.jsonl for writing.");
    }
}

Metrics::~Metrics() {
    if (logFile_.is_open()) {
        logFile_.close();
    }
}

void Metrics::RecordInference(const InferenceStats& stats) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile_.is_open()) {
        logFile_ << "{\"timestamp\":" << timestamp 
                 << ",\"latency_ms\":" << stats.latencyMs 
                 << ",\"input_mean\":" << stats.inputMean 
                 << ",\"input_stddev\":" << stats.inputStdDev 
                 << ",\"element_count\":" << stats.elementCount 
                 << "}\n";
    }
}

void Metrics::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logFile_.is_open()) {
        logFile_.flush();
    }
}
