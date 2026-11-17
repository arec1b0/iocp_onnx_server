#include "utils/Config.h"
#include "utils/Logger.h"
#include "inference/OnnxModel.h"
#include "server/IocpServer.h"
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

std::atomic<bool> g_Running{true};
static_assert(std::atomic<bool>::is_always_lock_free);

void SignalHandler(int) {
    g_Running.store(false, std::memory_order_relaxed);
}

int main() {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    const auto config = Config::Load();
    if (!config) {
        LOG_ERR("IOCP_ONNX_MAX_PAYLOAD_BYTES must be a decimal integer from " +
                std::to_string(Protocol::FloatBytes) + " to " + std::to_string(Protocol::MaxPayloadBytes) + ".");
        return 1;
    }
    
    LOG_INFO("Initializing ONNX Runtime model...");
    OnnxModel model;
    if (!model.Load(config->modelPath, config->maxPayloadBytes / Protocol::FloatBytes)) {
        LOG_ERR("Failed to load model.");
        return 1;
    }

    LOG_INFO("Starting IOCP Server...");
    IocpServer server(config->ipAddress, config->port, config->workerThreads, model.InputElementCount(),
                      [&model](const std::vector<float>& input) { return model.RunInference(input); });
    
    if (!server.Start()) {
        LOG_ERR("Failed to start server.");
        return 1;
    }

    while (g_Running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("Interrupt signal received. Shutting down...");
    server.Stop();
    LOG_INFO("Application terminated gracefully.");
    return 0;
}
