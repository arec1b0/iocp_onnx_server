#include "Logger.h"
#include <windows.h>
#include <winternl.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <ctime>
#include <limits>
#include <mutex>

#ifdef IOCP_TEST_LOG_WRITES
DWORD LimitTestLogWrite(DWORD requested);
#endif

namespace {
    constexpr std::size_t LineCapacity = 4096;
    enum class Delivery { Written, Partial, Dropped };

    bool IsSynchronous(HANDLE handle) {
        // FileModeInformation is the native query for the inherited open mode.
        // Win32's GetFileInformationByHandleEx does not support pipe handles.
        using QueryInformation = LONG(NTAPI*)(HANDLE, IO_STATUS_BLOCK*, void*, ULONG, ULONG);
        constexpr ULONG FileModeInformation = 16;
        constexpr ULONG SynchronousModes = 0x10 | 0x20;
        static const auto query = std::bit_cast<QueryInformation>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile"));
        IO_STATUS_BLOCK status{};
        ULONG mode = 0;
        return query && query(handle, &status, &mode, sizeof(mode), FileModeInformation) == 0 &&
            (mode & SynchronousModes) != 0;
    }

    struct Sink {
        explicit Sink(HANDLE target) : handle(target) {
            const DWORD type = GetFileType(handle);
            if (type == FILE_TYPE_PIPE) {
                available = IsSynchronous(handle);
                pipeNeedsMode = available;
            } else if (type == FILE_TYPE_DISK) {
                available = IsSynchronous(handle);
            } else if (type == FILE_TYPE_CHAR) {
                available = IsSynchronous(handle);
                DWORD consoleMode = 0;
                available = available || GetConsoleMode(handle, &consoleMode);
            }
        }

        bool FlushPartial() {
            if (remaining != 0) {
                const auto written = Write(tail.data() + offset, remaining);
                offset += written;
                remaining -= written;
            }
            return remaining == 0;
        }

        Delivery Submit(std::string_view line, bool countLoss = true) {
            if (line.size() >= LineCapacity || !FlushPartial()) {
                if (countLoss) CountDrop();
                return Delivery::Dropped;
            }
            std::copy(line.begin(), line.end(), tail.begin());
            tail[line.size()] = '\n';
            const auto length = line.size() + 1;
            offset = Write(tail.data(), length);
            if (offset == length) {
                return Delivery::Written;
            }
            if (offset == 0) {
                if (countLoss) CountDrop();
                return Delivery::Dropped;
            }
            remaining = length - offset;
            return Delivery::Partial;
        }

        bool HasPartial() const { return remaining != 0; }

        void CountDrop() {
            if (dropped != (std::numeric_limits<unsigned long long>::max)()) ++dropped;
        }

        HANDLE handle;
        unsigned long long dropped = 0;

    private:
        void RecordFailure(DWORD error) {
            available = error == ERROR_NO_DATA || error == ERROR_PIPE_BUSY;
        }

        std::size_t Write(const char* data, std::size_t size) {
            if (!available) return 0;
            if (pipeNeedsMode) {
                DWORD mode = PIPE_NOWAIT;
                if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) {
                    // A pipe can already be full on startup. Retry setup on the next record.
                    RecordFailure(GetLastError());
                    return 0;
                }
                pipeNeedsMode = false;
            }

            DWORD written = 0;
            auto requested = static_cast<DWORD>(size);
#ifdef IOCP_TEST_LOG_WRITES
            requested = LimitTestLogWrite(requested);
            if (requested == 0) return 0;
#endif
            if (!WriteFile(handle, data, requested, &written, nullptr)) {
                RecordFailure(GetLastError());
                return 0;
            }
            return written;
        }

        bool available = false;
        bool pipeNeedsMode = false;
        std::array<char, LineCapacity> tail{};
        std::size_t offset = 0;
        std::size_t remaining = 0;
    };

    struct Output {
        std::mutex mutex;
        Sink out{GetStdHandle(STD_OUTPUT_HANDLE)};
        Sink err{GetStdHandle(STD_ERROR_HANDLE)};
        Sink& errors = CompareObjectHandles(out.handle, err.handle) ? out : err;
        bool finished = false;
        bool deliveryReported = true;
    };

    Output& GetOutput() {
        static Output output;
        return output;
    }

    const char* LevelName(LogLevel level) {
        switch (level) {
            case LogLevel::INFO: return "INFO";
            case LogLevel::WARNING: return "WARN";
            case LogLevel::ERR: return "ERROR";
        }
        return "UNKNOWN";
    }
}

void Logger::Log(LogLevel level, std::string_view message) {
    constexpr std::string_view truncated = " [truncated]";
    std::array<char, LineCapacity> line;
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char timestamp[32]{};
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    const auto prefix = std::snprintf(line.data(), line.size(), "[%s] [%s] ", timestamp, LevelName(level));
    if (prefix < 0) return;
    const auto start = static_cast<std::size_t>(prefix);
    const auto capacity = line.size() - start - 1;
    const auto count = message.size() <= capacity ? message.size() : capacity - truncated.size();
    std::transform(message.begin(), message.begin() + count, line.begin() + start,
                   [](char c) { return c == '\r' || c == '\n' ? ' ' : c; });
    auto length = start + count;
    if (count != message.size()) {
        std::copy(truncated.begin(), truncated.end(), line.begin() + length);
        length += truncated.size();
    }

    auto& output = GetOutput();
    std::lock_guard lock(output.mutex);
    if (output.finished) return;
    const std::string_view record{line.data(), length};
    if (level == LogLevel::ERR) {
        if (output.errors.Submit(record) == Delivery::Dropped && &output.errors != &output.out) {
            output.out.Submit(record);
        }
    } else {
        output.out.Submit(record);
    }
}

void Logger::WriteJsonLine(std::string_view json) {
    auto& output = GetOutput();
    std::lock_guard lock(output.mutex);
    if (!output.finished) output.out.Submit(json);
}

bool Logger::Finish(std::string_view finalJson) {
    auto& output = GetOutput();
    std::lock_guard lock(output.mutex);
    if (output.finished) return output.deliveryReported;
    output.finished = true;
    output.out.FlushPartial();
    if (&output.errors != &output.out) output.errors.FlushPartial();

    bool finalWritten = finalJson.empty() || output.out.Submit(finalJson) == Delivery::Written;
    if (!finalWritten && &output.errors != &output.out) {
        finalWritten = output.errors.Submit(finalJson) == Delivery::Written;
    }
    if (output.out.dropped || output.err.dropped || output.out.HasPartial() || output.err.HasPartial()) {
        char summary[320];
        std::snprintf(summary, sizeof(summary),
                      "{\"event\":\"log_delivery\",\"stdout_dropped\":%llu,\"stderr_dropped\":%llu,"
                      "\"stdout_partial\":%s,\"stderr_partial\":%s,\"final_snapshot_delivered\":%s}",
                      output.out.dropped, output.err.dropped, output.out.HasPartial() ? "true" : "false",
                      output.err.HasPartial() ? "true" : "false", finalWritten ? "true" : "false");
        output.deliveryReported = (!output.errors.HasPartial() &&
                                    output.errors.Submit(summary, false) == Delivery::Written) ||
            (&output.errors != &output.out && !output.out.HasPartial() &&
             output.out.Submit(summary, false) == Delivery::Written);
    }
    return output.deliveryReported;
}
