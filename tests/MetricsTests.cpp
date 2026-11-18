#include "utils/Metrics.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
    void Check(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    std::string ReadFile(const std::filesystem::path& path) {
        std::ifstream file(path);
        Check(file.is_open(), "could not read metrics fixture");
        return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }

    struct TestFile {
        std::filesystem::path path;
        explicit TestFile(const char* caseName)
            : path("metrics-test-" + std::to_string(GetCurrentProcessId()) + "-" + caseName + ".jsonl") {}
        ~TestFile() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    };

    class CommaNumbers : public std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
        char do_thousands_sep() const override { return '.'; }
        std::string do_grouping() const override { return "\3"; }
    };

    class FaultBuffer : public std::streambuf {
    public:
        bool shortWrite = false;
        bool flushError = false;
        unsigned writes = 0;
        unsigned flushes = 0;
        std::string accepted;

    private:
        std::streamsize xsputn(const char* data, std::streamsize bytes) override {
            ++writes;
            const auto count = shortWrite ? (std::min)(std::streamsize{7}, bytes) : bytes;
            accepted.append(data, static_cast<std::size_t>(count));
            if (shortWrite) errno = ENOSPC;
            return count;
        }

        int sync() override {
            ++flushes;
            return flushError ? -1 : 0;
        }
    };

    constexpr InferenceStats Simple{1.25, -2.5f, 3.75f, 10};
}

struct MetricsTestAccess {
    static void Formatting() {
        std::array<char, Metrics::RecordCapacity> bytes;
        constexpr std::string_view expected =
            "{\"timestamp\":1234,\"latency_ms\":1.25,\"input_mean\":-2.5,\"input_stddev\":3.75,\"element_count\":10}\n";
        const auto original = std::locale::global(std::locale(std::locale::classic(), new CommaNumbers));
        const auto length = Metrics::FormatRecord(bytes, 1234, Simple);
        std::locale::global(original);
        Check(std::string_view(bytes.data(), length) == expected, "JSONL format changed with locale");
        Check(Metrics::FormatRecord(std::span(bytes).first(length), 1234, Simple) == length, "exact byte limit failed");
        Check(Metrics::FormatRecord(std::span(bytes).first(length - 1), 1234, Simple) == 0, "short buffer was accepted");
        Check(Metrics::FormatRecord({}, 1234, Simple) == 0, "empty format buffer was accepted");

        constexpr InferenceStats edges[] = {
            {(std::numeric_limits<double>::max)(), -(std::numeric_limits<float>::max)(),
             std::numeric_limits<float>::denorm_min(), (std::numeric_limits<std::size_t>::max)()},
            {std::numeric_limits<double>::denorm_min(), -0.0f, 0.0f, 1},
            {1234567.5, 0.0000009765625f, 123.125f, 1000}
        };
        constexpr auto timestamp = (std::numeric_limits<std::int64_t>::min)();
        for (const auto& stats : edges) {
            std::ostringstream legacy;
            legacy.imbue(std::locale::classic());
            legacy << "{\"timestamp\":" << timestamp << ",\"latency_ms\":" << stats.latencyMs
                   << ",\"input_mean\":" << stats.inputMean << ",\"input_stddev\":" << stats.inputStdDev
                   << ",\"element_count\":" << stats.elementCount << "}\n";
            const auto written = Metrics::FormatRecord(bytes, timestamp, stats);
            Check(std::string_view(bytes.data(), written) == legacy.str(), "finite number differs from the legacy formatter");
        }
        std::cout << "Exact JSONL, finite numeric extremes, locale and formatting boundaries passed.\n";
    }

    static void ConcurrentFile() {
        TestFile fixture("concurrent");
        constexpr int producers = 6;
        constexpr int perProducer = 140;
        Metrics sink(fixture.path);
        {
            std::vector<std::jthread> workers;
            for (int p = 0; p < producers; ++p) {
                workers.emplace_back([&, p] {
                    for (int i = 0; i < perProducer; ++i) {
                        const auto id = static_cast<std::size_t>(1 + p * perProducer + i);
                        sink.RecordInference({1.25, static_cast<float>(id), 3.75f, id});
                    }
                });
            }
        }
        const auto result = sink.Close();
        Check(result.Complete() && result.flushed == producers * perProducer, "concurrent record counts differ");
        std::ifstream file(fixture.path);
        std::array<bool, producers * perProducer> seen{};
        std::array<int, producers> latest{};
        for (std::string line; std::getline(file, line);) {
            const auto comma = line.find(',');
            const auto count = line.find("\"element_count\":");
            Check(line.starts_with("{\"timestamp\":") && comma != std::string::npos &&
                  count != std::string::npos, "truncated or interleaved record");
            const auto id = static_cast<std::size_t>(std::stoull(line.substr(count + 16)));
            Check(id >= 1 && id <= seen.size() && !seen[id - 1], "duplicate or invalid producer record");
            seen[id - 1] = true;
            const auto producer = (id - 1) / perProducer;
            Check(static_cast<int>(id) > latest[producer], "producer order changed");
            latest[producer] = static_cast<int>(id);
            const auto expected = ",\"latency_ms\":1.25,\"input_mean\":" + std::to_string(id) +
                ",\"input_stddev\":3.75,\"element_count\":" + std::to_string(id) + "}";
            Check(line.substr(comma) == expected, "producer output was malformed or interleaved");
        }
        Check(std::all_of(seen.begin(), seen.end(), [](bool present) { return present; }), "producer record is missing");
        std::cout << "840 concurrent records reconciled with exact fields and producer ordering.\n";
    }

    static void OpenFailureAndTail() {
        TestFile directory("open");
        Check(std::filesystem::create_directory(directory.path), "could not create invalid metrics path");
        Metrics bad(directory.path);
        bad.RecordInference(Simple);
        const auto open = bad.Close();
        Check(open.openFailed && open.skipped == 1 && open.appended == 0 && !open.Complete(), "open failure was unreported");

        TestFile tail("tail");
        { std::ofstream file(tail.path); file << "{\"timestamp\":"; }
        Metrics partial(tail.path);
        partial.RecordInference(Simple);
        Check(partial.Close().openFailed && ReadFile(tail.path) == "{\"timestamp\":", "append corrupted the existing partial tail");

        TestFile good("append");
        { std::ofstream file(good.path); file << "{\"legacy\":1}\n"; }
        Metrics healthy(good.path);
        healthy.RecordInference(Simple);
        Check(healthy.Close().Complete() && ReadFile(good.path).starts_with("{\"legacy\":1}\n{\"timestamp\":"),
              "a complete existing file could not be appended");
        std::cout << "Real unavailable path, preexisting partial tail and normal append passed.\n";
    }

    static void RejectedNumbers() {
        TestFile fixture("rejected");
        Metrics sink(fixture.path);
        sink.RecordInference({std::numeric_limits<double>::infinity(), 0, 0, 10});
        sink.RecordInference({1, std::numeric_limits<float>::quiet_NaN(), 0, 10});
        sink.RecordInference(Simple);
        const auto result = sink.Close();
        const auto text = ReadFile(fixture.path);
        Check(result.rejected == 2 && result.flushed == 1 && result.skipped == 0 && !result.Complete() &&
              std::count(text.begin(), text.end(), '\n') == 1 && text.ends_with(",\"element_count\":10}\n"),
              "non-finite stats were not reported and isolated");
    }

    static void PartialWriteAndProducers() {
        TestFile fixture("short");
        FaultBuffer fault;
        Metrics sink(fixture.path);
        static_cast<std::ostream&>(sink.logFile_).rdbuf(&fault);
        sink.RecordInference(Simple);
        fault.shortWrite = true;
        {
            std::vector<std::jthread> threads;
            for (int p = 0; p < 8; ++p) {
                threads.emplace_back([&] { for (int i = 0; i < 25; ++i) sink.RecordInference(Simple); });
            }
        }
        const auto result = sink.Close();
        Check(result.appended == 1 && result.flushed == 0 && result.writeFailures == 1 && result.skipped == 199 &&
              !result.closeFailed && !result.Complete() && fault.writes == 2 &&
              std::count(fault.accepted.begin(), fault.accepted.end(), '\n') == 1 && fault.accepted.ends_with("{\"times"),
              "simulated disk-full short write failed to fence concurrent producers");
        Check(sink.Close().writeFailures == 1, "repeated close altered failure accounting");
    }

    static void FlushAndCloseFailures() {
        {
            TestFile fixture("flush");
            FaultBuffer fault;
            Metrics sink(fixture.path);
            static_cast<std::ostream&>(sink.logFile_).rdbuf(&fault);
            sink.RecordInference(Simple);
            Check(sink.Flush(), "initial flush did not succeed");
            sink.RecordInference(Simple);
            fault.flushError = true;
            Check(!sink.Flush(), "a failed sync was reported as flushed");
            sink.RecordInference(Simple);
            const auto result = sink.Close();
            Check(result.flushed == 1 && result.appended == 2 && result.flushFailed && !result.closeFailed &&
                  result.skipped == 1 && !result.Complete() && fault.writes == 2, "flush failure did not persist");
        }
        {
            TestFile fixture("shutdown-flush");
            FaultBuffer fault;
            Metrics sink(fixture.path);
            static_cast<std::ostream&>(sink.logFile_).rdbuf(&fault);
            sink.RecordInference(Simple);
            fault.flushError = true;
            const auto result = sink.Close();
            Check(result.flushFailed && !result.closeFailed && result.flushed == 0 && !result.Complete(),
                  "shutdown flush failure was hidden");
        }
        {
            TestFile fixture("close");
            Metrics sink(fixture.path);
            sink.RecordInference(Simple);
            sink.failCloseForTest_ = true;
            const auto result = sink.Close();
            Check(result.closeFailed && !result.flushFailed && result.appended == 1 && result.flushed == 0 &&
                  !result.Complete() && sink.Close().closeFailed, "close failure was hidden");
        }
        std::cout << "Injected short write, persistent failure, concurrent suppression, flush and close failures passed.\n";
    }
};

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "faults") {
            MetricsTestAccess::OpenFailureAndTail();
            MetricsTestAccess::RejectedNumbers();
            MetricsTestAccess::PartialWriteAndProducers();
            MetricsTestAccess::FlushAndCloseFailures();
        } else {
            MetricsTestAccess::Formatting();
            MetricsTestAccess::ConcurrentFile();
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
