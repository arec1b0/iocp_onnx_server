#include "utils/Logger.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <latch>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
    constexpr DWORD UnlimitedWrite = (std::numeric_limits<DWORD>::max)();
    DWORD writeLimit = UnlimitedWrite;

    void Check(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    class Pipe {
    public:
        Pipe() { Check(CreatePipe(&read_, &write_, nullptr, 4096) != FALSE, "CreatePipe failed"); }
        ~Pipe() { CloseReader(); CloseHandle(write_); }
        Pipe(const Pipe&) = delete;
        Pipe& operator=(const Pipe&) = delete;

        HANDLE Writer() const { return write_; }

        void CloseReader() {
            if (read_) {
                CloseHandle(read_);
                read_ = nullptr;
            }
        }

        DWORD Available() const {
            DWORD available = 0;
            Check(PeekNamedPipe(read_, nullptr, 0, nullptr, &available, nullptr) != FALSE, "PeekNamedPipe failed");
            return available;
        }

        DWORD Fill() const {
            DWORD mode = PIPE_NOWAIT;
            Check(SetNamedPipeHandleState(write_, &mode, nullptr, nullptr) != FALSE, "could not configure fixture pipe");
            DWORD outbound = 0, inbound = 0;
            Check(GetNamedPipeInfo(read_, nullptr, &outbound, &inbound, nullptr) != FALSE, "GetNamedPipeInfo failed");
            const auto capacity = inbound + outbound;
            const std::array<char, 256> filler{};
            Check(capacity >= 4096 && capacity % filler.size() == 0, "Unexpected pipe capacity");
            DWORD total = 0;
            while (total < capacity) {
                DWORD written = 0;
                Check(WriteFile(write_, filler.data(), static_cast<DWORD>(filler.size()), &written, nullptr) != FALSE,
                      "could not prefill test pipe");
                if (!written) break;
                total += written;
            }
            Check(total > 256 && Available() == total, "expected a full test pipe");
            mode = PIPE_WAIT;
            Check(SetNamedPipeHandleState(write_, &mode, nullptr, nullptr) != FALSE, "could not restore blocking mode");
            return total;
        }

        std::string Read(DWORD size) const {
            Check(size <= Available(), "attempted to read unavailable bytes");
            std::string result(size, '\0');
            DWORD count = 0;
            if (size != 0) {
                Check(ReadFile(read_, result.data(), size, &count, nullptr) && count == size, "could not read test pipe");
            }
            return result;
        }

    private:
        HANDLE read_ = nullptr;
        HANDLE write_ = nullptr;
    };

    void Redirect(const Pipe& out, const Pipe& err) {
        Check(SetStdHandle(STD_OUTPUT_HANDLE, out.Writer()) != FALSE &&
                  SetStdHandle(STD_ERROR_HANDLE, err.Writer()) != FALSE, "could not redirect test stdout and stderr");
    }

    constexpr std::string_view Final = R"({"event":"connection_metrics","phase":"stopped"})";
    constexpr std::string_view Next = R"({"event":"test","sequence":2})";

    void CheckPartial(bool breakPipe) {
        Pipe out, err;
        Redirect(out, err);
        if (!breakPipe) {
            const auto capacity = out.Fill();
            Logger::WriteJsonLine(R"({"event":"dropped_while_full"})");
            Check(out.Read(out.Available()) == std::string(capacity, '\0'), "full pipe accepted a new record");
        }
        const auto line = std::string(R"({"text":")") + std::string(1400, 'q') + "\"}\n";
        // Force a short native write and then a period without progress.
        writeLimit = 256;
        Logger::WriteJsonLine(std::string_view(line).substr(0, line.size() - 1));
        const auto buffered = out.Available();
        Check(buffered == 256, "did not observe the forced partial JSON record");
        writeLimit = 0;
        Logger::WriteJsonLine(R"({"event":"dropped_while_full"})");
        writeLimit = UnlimitedWrite;

        if (breakPipe) {
            out.CloseReader();
            Check(Logger::Finish(Final), "the loss could not be reported over stderr");
            const auto notices = err.Read(err.Available());
            Check(notices.starts_with(std::string(Final) + '\n') && notices.find(R"("stdout_partial":true)") != std::string::npos,
                  "incomplete record and final fallback were not reported");
        } else {
            const auto prefix = out.Read(buffered);
            Logger::WriteJsonLine(Next);
            Check(Logger::Finish(Final), "could not report the record lost to backpressure");
            Check(prefix + out.Read(out.Available()) == line + std::string(Next) + '\n' + std::string(Final) + '\n',
                  "slow consumer received invalid, reordered, or interleaved records");
            const auto notices = err.Read(err.Available());
            Check(notices.find(R"("stdout_dropped":2)") != std::string::npos &&
                      notices.find(R"("stdout_partial":false)") != std::string::npos, "unexpected slow-reader loss report");
        }
        Check(Logger::Finish(Final) && err.Available() == 0, "repeated finalization duplicated diagnostics");
        std::cout << "{\"event\":\"logger_test\",\"partial_bytes\":" << buffered
                  << ",\"pipe_broken\":" << (breakPipe ? "true" : "false") << "}\n";
    }

    SIZE_T PrivateBytes() {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        Check(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                      sizeof(counters)) != FALSE, "could not read process private bytes");
        return counters.PrivateUsage;
    }

    void CheckPressure() {
        Pipe out, err;
        out.Fill();
        Redirect(out, err);
        const auto before = PrivateBytes();
        constexpr int Count = 4;
        constexpr int Rounds = 50000;
        std::latch ready(Count);
        std::latch finish(1);
        std::array<std::jthread, Count> producers;
        for (auto& producer : producers) {
            producer = std::jthread([&] {
                for (int i = 0; i < Rounds; ++i) Logger::WriteJsonLine(Next);
                ready.count_down();
                finish.wait();
                for (int i = 0; i < Rounds; ++i) Logger::WriteJsonLine(Next);
            });
        }
        ready.wait();
        const auto underPressure = PrivateBytes();
        finish.count_down();
        Check(Logger::Finish(Final), "pressure prevented final fallback");
        for (auto& producer : producers) producer.join();
        const auto notices = err.Read(err.Available());
        Check(notices.starts_with(std::string(Final) + '\n') && notices.find(R"("stdout_partial":false)") != std::string::npos,
              "pressure did not leave a parseable final record and loss notice");
        const auto loss = notices.find(R"("stdout_dropped":)");
        Check(loss != std::string::npos && std::stoull(notices.substr(loss + std::string_view(R"("stdout_dropped":)").size())) >= Count * Rounds,
              "pressure did not account for refused records");
        Check(underPressure <= before + 16 * 1024 * 1024, "excessive memory growth under stdout pressure");
        std::cout << "{\"event\":\"logger_test\",\"records_under_pressure\":" << Count * Rounds
                  << ",\"initial_private_bytes\":" << before << ",\"pressure_private_bytes\":" << underPressure << "}\n";
    }

    void CheckUnavailable() {
        Pipe shared;
        shared.Fill();
        Redirect(shared, shared);
        Logger::Log(LogLevel::ERR, "critical diagnostic");
        Check(!Logger::Finish(Final) && !Logger::Finish(), "unobservable losses were reported as delivered");
        std::cout << "{\"event\":\"logger_test\",\"loss_report_delivered\":false}\n";
    }
}

DWORD LimitTestLogWrite(DWORD requested) {
    return (std::min)(requested, writeLimit);
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "expected logger test case");
        const std::string_view test = argv[1];
        if (test == "slow") CheckPartial(false);
        else if (test == "broken") CheckPartial(true);
        else if (test == "pressure") CheckPressure();
        else if (test == "unavailable") CheckUnavailable();
        else Check(false, "unknown logger test case");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
