#pragma once
#include <cstdint>

/// Consistent aggregate snapshot. Cumulative counters saturate and persist
/// across Start and Stop on the same server instance.
struct ConnectionMetricsSnapshot {
    std::uint64_t connections_accepted_total = 0;
    std::uint64_t connections_rejected_capacity_total = 0;
    std::uint64_t connections_closed_total = 0;
    std::uint64_t connections_active = 0;
    std::uint64_t connections_high_watermark = 0;
    std::uint64_t idle_timeouts_total = 0;
    std::uint64_t incomplete_frame_timeouts_total = 0;
    std::uint64_t send_timeouts_total = 0;
    std::uint64_t io_failures_total = 0;
    std::uint64_t pending_io_operations = 0;
    std::uint64_t cancellation_requests_total = 0;
    std::uint64_t cancellation_completions_total = 0;
};
