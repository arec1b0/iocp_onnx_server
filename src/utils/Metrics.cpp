#include "Metrics.h"
#include "Logger.h"
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace {
    class RecordWriter {
    public:
        explicit RecordWriter(std::span<char> buffer) : remaining_(buffer) {}

        bool Text(std::string_view text) noexcept {
            if (text.size() > remaining_.size()) return false;
            std::memcpy(remaining_.data(), text.data(), text.size());
            remaining_ = remaining_.subspan(text.size());
            return true;
        }

        template<class T>
        bool Number(T value) noexcept {
            const auto first = remaining_.data();
            const auto last = first + remaining_.size();
            std::to_chars_result result;
            if constexpr (std::is_floating_point_v<T>) {
                // Match defaultfloat's six significant digits independently of locale.
                result = std::to_chars(first, last, value, std::chars_format::general, 6);
            } else {
                result = std::to_chars(first, last, value);
            }
            if (result.ec != std::errc{}) return false;
            remaining_ = remaining_.subspan(static_cast<std::size_t>(result.ptr - first));
            return true;
        }

        std::size_t Remaining() const noexcept { return remaining_.size(); }

    private:
        std::span<char> remaining_;
    };

    bool CanAppend(const std::filesystem::path& path) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error || !exists) return !error;

        // A prior short write can leave an unterminated line. Keep it intact
        // for inspection and avoid joining it to an otherwise valid record.
        std::ifstream previous(path, std::ios::binary | std::ios::ate);
        if (!previous) return false;
        if (previous.tellg() == 0) return true;
        previous.seekg(-1, std::ios::end);
        return previous.get() == '\n';
    }

    void Increment(std::uint64_t& count) noexcept {
        if (count != (std::numeric_limits<std::uint64_t>::max)()) ++count;
    }
}

bool Metrics::Delivery::Complete() const noexcept {
    return appended == flushed && !rejected && !writeFailures && !skipped && !openFailed && !flushFailed && !closeFailed;
}

Metrics& Metrics::GetInstance() {
    static Metrics instance("metrics.jsonl");
    return instance;
}

Metrics::Metrics(const std::filesystem::path& path) {
    if (CanAppend(path)) logFile_.open(path, std::ios::app);
    delivery_.openFailed = !logFile_.is_open();
    if (delivery_.openFailed) LOG_ERR("Could not open metrics.jsonl for safe append. Check access and any existing final record.");
}

std::size_t Metrics::FormatRecord(std::span<char> buffer, std::int64_t timestamp, const InferenceStats& stats) noexcept {
    if (buffer.empty() || !std::isfinite(stats.latencyMs) || !std::isfinite(stats.inputMean) ||
        !std::isfinite(stats.inputStdDev)) return 0;

    RecordWriter record(buffer);
    if (!record.Text("{\"timestamp\":") || !record.Number(timestamp) ||
        !record.Text(",\"latency_ms\":") || !record.Number(stats.latencyMs) ||
        !record.Text(",\"input_mean\":") || !record.Number(stats.inputMean) ||
        !record.Text(",\"input_stddev\":") || !record.Number(stats.inputStdDev) ||
        !record.Text(",\"element_count\":") || !record.Number(stats.elementCount) ||
        !record.Text("}\n")) return 0;
    return buffer.size() - record.Remaining();
}

bool Metrics::FailedLocked() const noexcept {
    return delivery_.openFailed || delivery_.writeFailures || delivery_.flushFailed || delivery_.closeFailed;
}

void Metrics::RecordInference(const InferenceStats& stats, InferenceProfile::Counters* profile) {
    std::array<char, RecordCapacity> record;
    std::size_t length;
    {
        InferenceProfile::Timer format(profile, InferenceProfile::MetricsFormat);
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        length = FormatRecord(record, timestamp, stats);
    }

    const char* diagnostic = nullptr;
    const auto waitingSince = InferenceProfile::Stamp(profile);
    {
        std::lock_guard lock(mutex_);
        if (profile) profile->Record(InferenceProfile::MetricsLockWait, waitingSince);
        InferenceProfile::Timer hold(profile, InferenceProfile::MetricsAppendHold);
        if (closed_ || FailedLocked()) {
            Increment(delivery_.skipped);
            return;
        }
        if (length == 0) {
            if (!delivery_.rejected) diagnostic = "Inference metrics record rejected. A number was non-finite or the record exceeded its buffer.";
            Increment(delivery_.rejected);
        } else if (!logFile_.write(record.data(), static_cast<std::streamsize>(length))) {
            Increment(delivery_.writeFailures);
            diagnostic = "Inference metrics append failed. The file may contain an incomplete final record.";
        } else {
            Increment(delivery_.appended);
        }
    }
    if (diagnostic) LOG_ERR(diagnostic);
}

bool Metrics::FlushLocked() {
    if (FailedLocked()) return false;
    if (logFile_.flush()) return true;
    delivery_.flushFailed = true;
    return false;
}

bool Metrics::Flush() {
    {
        std::lock_guard lock(mutex_);
        if (closed_ || FailedLocked()) return false;
        if (FlushLocked()) {
            delivery_.flushed = delivery_.appended;
            return true;
        }
    }
    LOG_ERR("Inference metrics flush failed. Further records will be counted as skipped.");
    return false;
}

Metrics::Delivery Metrics::Close() {
    std::lock_guard lock(mutex_);
    if (closed_) return delivery_;
    closed_ = true;

    if (logFile_.is_open()) {
        const bool flushed = FlushLocked();
        logFile_.clear(); // Distinguish a new close failure from an earlier stream failure.
        logFile_.close();
#ifdef IOCP_TEST_METRICS
        if (failCloseForTest_) logFile_.setstate(std::ios::failbit);
#endif
        delivery_.closeFailed = logFile_.fail();
        if (flushed && !delivery_.closeFailed) delivery_.flushed = delivery_.appended;
    }
    return delivery_;
}
