#include "InferenceProfile.h"
#include <algorithm>
#include <fstream>

void InferenceProfile::Counters::Record(Phase phase, Clock::time_point start) noexcept {
    const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - start).count());
    auto& duration = durations[phase];
    ++duration.count;
    duration.total += ns;
    duration.max = (std::max)(duration.max, ns);
}

void InferenceProfile::Counters::AdmissionDepth(std::uint64_t depth) noexcept {
    ++admitted;
    depthSum += depth;
    depthMax = (std::max)(depthMax, depth);
}

InferenceProfile::Recorder::~Recorder() {
    if (profile_) profile_->Merge(local_);
}

InferenceProfile::RegistryLock::RegistryLock(std::mutex& mutex, Counters* counters, Phase wait, Phase hold)
    : counters_(counters), hold_(hold), acquired_(Stamp(counters)), lock_(mutex) {
    if (counters_) {
        counters_->Record(wait, acquired_);
        acquired_ = Clock::now();
    }
}

InferenceProfile::RegistryLock::~RegistryLock() {
    if (lock_.owns_lock()) Unlock();
}

void InferenceProfile::RegistryLock::Unlock() {
    if (counters_) counters_->Record(hold_, acquired_);
    lock_.unlock();
}

void InferenceProfile::Merge(const Counters& local) {
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < total_.durations.size(); ++i) {
        auto& total = total_.durations[i];
        const auto& part = local.durations[i];
        total.count += part.count;
        total.total += part.total;
        total.max = (std::max)(total.max, part.max);
    }
    total_.admitted += local.admitted;
    total_.rejected += local.rejected;
    total_.cancelled += local.cancelled;
    total_.finished += local.finished;
    total_.depthSum += local.depthSum;
    total_.depthMax = (std::max)(total_.depthMax, local.depthMax);
    total_.waitReturns += local.waitReturns;
    total_.emptyWaitReturns += local.emptyWaitReturns;
}

bool InferenceProfile::Save() const {
    constexpr std::array names{
        "frame", "admission", "insertion", "queue_wait", "removal", "execute", "metrics", "prepare", "dispatch",
        "claim_socket", "dispatch_socket", "sweep_socket", "receive_post", "send_post", "response", "worker_busy",
        "worker_lifetime",
        "completion_wait", "completion_hold", "handoff_wait", "handoff_hold", "claim_wait", "claim_hold",
        "dispatch_wait", "dispatch_hold", "submit_result_wait", "submit_result_hold", "early_completion_wait",
        "accept_wait", "accept_hold", "sweep_wait", "sweep_hold", "metrics_format", "metrics_lock_wait", "metrics_append_hold"
    };
    static_assert(names.size() == PhaseCount);
    std::ofstream output("inference-profile.json", std::ios::trunc);
    if (!output) return false;
    output << "{\"admitted\":" << total_.admitted << ",\"rejected\":" << total_.rejected
           << ",\"cancelled\":" << total_.cancelled << ",\"finished\":" << total_.finished
           << ",\"queue_depth_sum_at_admission\":" << total_.depthSum
           << ",\"queue_high_watermark\":" << total_.depthMax
           << ",\"wait_returns\":" << total_.waitReturns
           << ",\"empty_wait_returns\":" << total_.emptyWaitReturns << ",\"timings\":{";
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto& duration = total_.durations[i];
        if (i != 0) output << ',';
        output << '"' << names[i] << "\":{\"count\":" << duration.count
               << ",\"total_ns\":" << duration.total << ",\"max_ns\":" << duration.max << '}';
    }
    output << "}}\n";
    output.close();
    return !output.fail();
}
