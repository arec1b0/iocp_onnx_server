#include "Config.h"
#include <thread>

Config Config::Load() {
    Config config;
    config.modelPath = "models/model.onnx";
    config.ipAddress = "0.0.0.0";
    config.port = 8080;
    
    unsigned int hardware_threads = std::thread::hardware_concurrency();
    config.workerThreads = hardware_threads > 0 ? hardware_threads : 2;
    
    return config;
}