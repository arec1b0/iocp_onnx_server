#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include "inference/InferenceProfile.h"

/// Per-inference measurements recorded to the metrics log.
///
/// inputMean/inputStdDev capture the input distribution so that data drift
/// shows up as a shift in the logged series (see docs/ARCHITECTURE.md,
/// "Observability").
struct InferenceStats {
    double latencyMs;    ///< Wall-clock duration of RunInference only.
    float inputMean;     ///< Mean of the input tensor.
    float inputStdDev;   ///< Standard deviation of the input tensor.
    size_t elementCount; ///< Input tensor size in elements.
};

/// Mutex-guarded singleton that appends one JSON line per inference to
/// metrics.jsonl (opened in append mode in the process working directory).
/// Consumed by scripts/analyze_metrics.py.
class Metrics {
public:
    struct Delivery {
        std::uint64_t appended = 0;
        std::uint64_t flushed = 0;
        std::uint64_t rejected = 0;
        std::uint64_t writeFailures = 0;
        std::uint64_t skipped = 0;
        bool openFailed = false;
        bool flushFailed = false;
        bool closeFailed = false;

        bool Complete() const noexcept;
    };

    static Metrics& GetInstance();

    /// Appends a timestamped JSONL record. Thread-safe.
    void RecordInference(const InferenceStats& stats, InferenceProfile::Counters* profile = nullptr);

    /// Flushes buffered records to the OS. Thread-safe.
    bool Flush();

    /// Call after producers have joined. Repeated calls perform no additional I/O.
    Delivery Close();

private:
    friend struct MetricsTestAccess;

    explicit Metrics(const std::filesystem::path& path);
    ~Metrics() = default;
    static constexpr std::size_t RecordCapacity = 256;
    static std::size_t FormatRecord(std::span<char> buffer, std::int64_t timestamp, const InferenceStats& stats) noexcept;
    bool FlushLocked();
    bool FailedLocked() const noexcept;

    std::mutex mutex_;
    std::ofstream logFile_;
    Delivery delivery_;
    bool closed_ = false;
#ifdef IOCP_TEST_METRICS
    bool failCloseForTest_ = false;
#endif
};
