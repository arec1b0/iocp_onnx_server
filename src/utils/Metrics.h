#pragma once
#include <vector>
#include <string>
#include <fstream>
#include <mutex>

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
    static Metrics& GetInstance();

    /// Appends a timestamped JSONL record. Thread-safe.
    void RecordInference(const InferenceStats& stats);

    /// Flushes the log file to disk. Thread-safe.
    void Flush();

private:
    Metrics();
    ~Metrics();

    std::mutex mutex_;
    std::ofstream logFile_;
};
