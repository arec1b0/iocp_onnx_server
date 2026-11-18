// Diagnostic-only replacement for Metrics.cpp in the isolated benchmark build.
#include "Metrics.h"
#include "Logger.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <streambuf>
#include <string_view>

namespace {
    using Clock = std::chrono::steady_clock;

    enum class Sink { File, NullDevice, Memory, Bypass };

    class DiscardBuffer : public std::streambuf {
        std::streamsize xsputn(const char*, std::streamsize count) override { return count; }
        int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }
    };

    struct Probe {
        Sink sink = Sink::File;
        bool split = false;
        DiscardBuffer buffer;
        std::ostream memory{&buffer};
        std::atomic<std::uint64_t> bypassRecords{0};
        std::uint64_t records = 0;
        std::uint64_t lockWaitNs = 0;
        std::uint64_t streamNs = 0;
        std::uint64_t explicitFlushes = 0;
        std::uint64_t flushNs = 0;

        Probe() {
            char mode[16]{};
            if (const auto length = GetEnvironmentVariableA("IOCP_BENCH_METRICS_MODE", mode, sizeof(mode));
                length && length < sizeof(mode)) {
                const std::string_view value(mode, length);
                if (value == "null") sink = Sink::NullDevice;
                if (value == "memory") sink = Sink::Memory;
                if (value == "bypass") sink = Sink::Bypass;
            }
            char flag[2]{};
            split = GetEnvironmentVariableA("IOCP_BENCH_METRICS_SPLIT", flag, sizeof(flag)) == 1 && flag[0] == '1';
        }
    };

    Probe& GetProbe() {
        static Probe probe;
        return probe;
    }

    std::uint64_t Elapsed(Clock::time_point start, Clock::time_point end) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    }

    void WriteRecord(std::ostream& stream, long long timestamp, const InferenceStats& stats) {
        stream << "{\"timestamp\":" << timestamp
               << ",\"latency_ms\":" << stats.latencyMs
               << ",\"input_mean\":" << stats.inputMean
               << ",\"input_stddev\":" << stats.inputStdDev
               << ",\"element_count\":" << stats.elementCount
               << "}\n";
    }
}

Metrics& Metrics::GetInstance() {
    static Metrics instance;
    return instance;
}

Metrics::Metrics() {
    const auto sink = GetProbe().sink;
    if (sink == Sink::File || sink == Sink::NullDevice) {
        logFile_.open(sink == Sink::File ? "metrics.jsonl" : "NUL", std::ios::app);
        if (!logFile_.is_open()) LOG_ERR("Failed to open benchmark metrics sink.");
    }
}

Metrics::~Metrics() {
    const auto& probe = GetProbe();
    const auto closeStarted = probe.split ? Clock::now() : Clock::time_point{};
    if (logFile_.is_open()) logFile_.close();
    if (probe.split) {
        const auto closeNs = Elapsed(closeStarted, Clock::now());
        std::ofstream output("metrics-sink-profile.json", std::ios::trunc);
        output << "{\"records\":" << probe.records + probe.bypassRecords.load(std::memory_order_relaxed)
               << ",\"lock_wait_ns\":" << probe.lockWaitNs << ",\"stream_ns\":" << probe.streamNs
               << ",\"explicit_flushes\":" << probe.explicitFlushes << ",\"flush_ns\":" << probe.flushNs
               << ",\"close_ns\":" << closeNs << "}\n";
    }
}

void Metrics::RecordInference(const InferenceStats& stats) {
    auto& probe = GetProbe();
    if (probe.sink == Sink::Bypass) {
        if (probe.split) probe.bypassRecords.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const auto lockStarted = probe.split ? Clock::now() : Clock::time_point{};
    std::lock_guard lock(mutex_);
    const auto acquired = probe.split ? Clock::now() : Clock::time_point{};
    if (probe.sink == Sink::Memory) {
        WriteRecord(probe.memory, timestamp, stats);
    } else if (logFile_.is_open()) {
        WriteRecord(logFile_, timestamp, stats);
    }
    if (probe.split) {
        probe.lockWaitNs += Elapsed(lockStarted, acquired);
        probe.streamNs += Elapsed(acquired, Clock::now());
        ++probe.records;
    }
}

void Metrics::Flush() {
    auto& probe = GetProbe();
    std::lock_guard lock(mutex_);
    const auto started = probe.split ? Clock::now() : Clock::time_point{};
    if (logFile_.is_open()) logFile_.flush();
    if (probe.split) {
        ++probe.explicitFlushes;
        probe.flushNs += Elapsed(started, Clock::now());
    }
}
