#include "utils/Config.h"
#include "utils/Logger.h"
#include "utils/Metrics.h"
#include "inference/OnnxModel.h"
#include "server/IocpServer.h"
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

std::atomic<bool> g_Running{true};
static_assert(std::atomic<bool>::is_always_lock_free);

void SignalHandler(int) {
    g_Running.store(false, std::memory_order_relaxed);
}

namespace {
    constexpr auto ConnectionMetricsInterval = std::chrono::seconds(30);
    constexpr int MetricsDeliveryFailed = 4;

    void ReportInferenceDeliveryFailure(const Metrics::Delivery& delivery) {
        char message[512];
        const auto size = std::snprintf(message, sizeof(message),
            "{\"event\":\"inference_metrics_delivery_failed\",\"appended\":%llu,\"flushed\":%llu,"
            "\"unconfirmed\":%llu,\"rejected\":%llu,\"write_failures\":%llu,\"skipped\":%llu,"
            "\"open_failed\":%s,\"flush_failed\":%s,\"close_failed\":%s}",
            static_cast<unsigned long long>(delivery.appended), static_cast<unsigned long long>(delivery.flushed),
            static_cast<unsigned long long>(delivery.appended - delivery.flushed),
            static_cast<unsigned long long>(delivery.rejected), static_cast<unsigned long long>(delivery.writeFailures),
            static_cast<unsigned long long>(delivery.skipped), delivery.openFailed ? "true" : "false",
            delivery.flushFailed ? "true" : "false", delivery.closeFailed ? "true" : "false");
        if (size > 0 && static_cast<std::size_t>(size) < sizeof(message)) LOG_ERR(message);
    }

    std::string SerializeConnectionMetrics(const IocpServer& server, const char* phase) {
        const auto snapshot = server.GetConnectionMetrics();
        const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::ostringstream json;
        json << "{\"event\":\"connection_metrics\",\"phase\":\"" << phase << "\""
             << ",\"timestamp_ms\":" << timestamp
             << ",\"connections_accepted_total\":" << snapshot.connections_accepted_total
             << ",\"connections_rejected_capacity_total\":" << snapshot.connections_rejected_capacity_total
             << ",\"connections_closed_total\":" << snapshot.connections_closed_total
             << ",\"connections_active\":" << snapshot.connections_active
             << ",\"connections_high_watermark\":" << snapshot.connections_high_watermark
             << ",\"idle_timeouts_total\":" << snapshot.idle_timeouts_total
             << ",\"incomplete_frame_timeouts_total\":" << snapshot.incomplete_frame_timeouts_total
             << ",\"send_timeouts_total\":" << snapshot.send_timeouts_total
             << ",\"io_failures_total\":" << snapshot.io_failures_total
             << ",\"pending_io_operations\":" << snapshot.pending_io_operations
             << ",\"cancellation_requests_total\":" << snapshot.cancellation_requests_total
             << ",\"cancellation_completions_total\":" << snapshot.cancellation_completions_total
             << '}';
        return std::move(json).str();
    }
}

static int RunServer(std::string& finalMetrics) {
    const auto config = Config::Load();
    if (!config) {
        return 1;
    }
    
    LOG_INFO("Initializing ONNX Runtime model...");
    OnnxModel model;
    if (!model.Load(config->modelPath, config->maxPayloadBytes / Protocol::FloatBytes)) {
        LOG_ERR("Failed to load model.");
        return 1;
    }

    LOG_INFO("Starting IOCP Server...");
    const auto trace = config->connectionTrace ? std::make_unique<ConnectionTrace>() : nullptr;
    const auto profile = config->inferenceProfile ? std::make_unique<InferenceProfile>() : nullptr;
    IocpServer server(config->ipAddress, config->port, config->workerThreads, model.InputElementCount(),
                      [&model](const std::vector<float>& input) { return model.RunInference(input); },
                      config->connectionLimits, IocpServer::Clock::now, trace.get(), config->inferenceLimits,
                      profile.get());
    
    if (!server.Start()) {
        LOG_ERR("Failed to start server.");
        return 1;
    }
    auto& metrics = Metrics::GetInstance();

    Logger::WriteJsonLine(SerializeConnectionMetrics(server, "running"));
    auto nextReport = IocpServer::Clock::now() + ConnectionMetricsInterval;
    while (g_Running) {
        const auto now = IocpServer::Clock::now();
        if (now >= nextReport) {
            Logger::WriteJsonLine(SerializeConnectionMetrics(server, "running"));
            nextReport = now + ConnectionMetricsInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.Stop();
    const auto delivery = metrics.Close();
    finalMetrics = SerializeConnectionMetrics(server, "stopped");
    if (trace && !trace->Save()) LOG_ERR("Could not save connection-trace.jsonl.");
    if (profile && !profile->Save()) LOG_ERR("Could not save inference-profile.json.");
    if (delivery.Complete()) {
        LOG_INFO("Application terminated gracefully.");
        return 0;
    }
    ReportInferenceDeliveryFailure(delivery);
    return MetricsDeliveryFailed;
}

int main() {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    std::string finalMetrics;
    const int status = RunServer(finalMetrics);
    if (Logger::Finish(finalMetrics)) return status;
    // The process exit status remains observable when both log sinks are unavailable.
    return status == 0 ? 2 : 3;
}
