#pragma once
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Optional, fixed-size timing aggregates. Threads publish only as they exit.
class InferenceProfile {
public:
    using Clock = std::chrono::steady_clock;
    enum Phase {
        Frame, Admission, Insertion, QueueWait, Removal, Execute, Metrics, Prepare, Dispatch,
        ClaimSocket, DispatchSocket, SweepSocket, ReceivePost, SendPost, Response, WorkerBusy, WorkerLifetime,
        CompletionWait, CompletionHold, HandoffWait, HandoffHold, ClaimWait, ClaimHold,
        DispatchWait, DispatchHold, SubmitResultWait, SubmitResultHold, EarlyCompletionWait,
        AcceptWait, AcceptHold, SweepWait, SweepHold, PhaseCount
    };

    struct Duration {
        std::uint64_t count = 0;
        std::uint64_t total = 0;
        std::uint64_t max = 0;
    };

    struct Counters {
        std::array<Duration, PhaseCount> durations{};
        std::uint64_t admitted = 0;
        std::uint64_t rejected = 0;
        std::uint64_t cancelled = 0;
        std::uint64_t finished = 0;
        std::uint64_t depthSum = 0;
        std::uint64_t depthMax = 0;
        std::uint64_t waitReturns = 0;
        std::uint64_t emptyWaitReturns = 0;

        void Record(Phase phase, Clock::time_point start) noexcept;
        void AdmissionDepth(std::uint64_t depth) noexcept;
    };

    static Clock::time_point Stamp(const Counters* counters) noexcept {
        return counters ? Clock::now() : Clock::time_point{};
    }

    class Recorder {
    public:
        explicit Recorder(InferenceProfile* profile) : profile_(profile) {}
        ~Recorder();
        Counters* Get() noexcept { return profile_ ? &local_ : nullptr; }
    private:
        InferenceProfile* profile_;
        Counters local_;
    };

    class Timer {
    public:
        Timer(Counters* counters, Phase phase) noexcept : counters_(counters), phase_(phase), start_(Stamp(counters)) {}
        ~Timer() { if (counters_) counters_->Record(phase_, start_); }
    private:
        Counters* counters_;
        Phase phase_;
        Clock::time_point start_;
    };

    class RegistryLock {
    public:
        RegistryLock(std::mutex& mutex, Counters* counters, Phase wait, Phase hold);
        ~RegistryLock();
        void Unlock();

        template<class Ready>
        void Wait(std::condition_variable& cv, Ready ready, bool countWakeups = true) {
            if (!counters_) {
                cv.wait(lock_, ready);
                return;
            }
            while (!ready()) {
                counters_->Record(hold_, acquired_);
                cv.wait(lock_);
                acquired_ = Clock::now();
                if (countWakeups) {
                    ++counters_->waitReturns;
                    if (!ready()) ++counters_->emptyWaitReturns;
                }
            }
        }
    private:
        Counters* counters_;
        Phase hold_;
        Clock::time_point acquired_;
        std::unique_lock<std::mutex> lock_;
    };

    // Call after Stop has joined the producers. The JSON contains aggregates only.
    bool Save() const;

private:
    void Merge(const Counters& local);
    std::mutex mutex_;
    Counters total_;
};
