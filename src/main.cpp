#include "utils/Config.h"
#include "utils/Logger.h"
#include "inference/OnnxModel.h"
#include "server/IocpServer.h"
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>

std::atomic<bool> g_Running{true};

void SignalHandler(int signum) {
    LOG_INFO("Interrupt signal received. Shutting down...");
    g_Running = false;
}

int main() {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    Config config = Config::Load();
    
    LOG_INFO("Initializing ONNX Runtime model...");
    OnnxModel model;
    if (!model.Load(config.modelPath)) {
        LOG_ERR("Failed to load model. Ensure the path is correct.");
        return 1;
    }

    LOG_INFO("Starting IOCP Server...");
    IocpServer server(config.ipAddress, config.port, config.workerThreads, &model);
    
    if (!server.Start()) {
        LOG_ERR("Failed to start server.");
        return 1;
    }

    while (g_Running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.Stop();
    LOG_INFO("Application terminated gracefully.");
    return 0;
}