#include "Config.h"
#include "server/Protocol.h"
#include <windows.h>
#include <charconv>
#include <thread>

std::optional<Config> Config::Load() {
    Config config;
    config.modelPath = "models/model.onnx";
    config.ipAddress = "0.0.0.0";
    config.port = 8080;
    
    unsigned int hardware_threads = std::thread::hardware_concurrency();
    config.workerThreads = hardware_threads > 0 ? hardware_threads : 2;

    config.maxPayloadBytes = Protocol::DefaultMaxPayloadBytes;
    char limit[11]; // Ten decimal digits and a terminator for uint32_t.
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableA("IOCP_ONNX_MAX_PAYLOAD_BYTES", limit, sizeof(limit));
    if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        return config;
    }
    if (length == 0 || length >= sizeof(limit)) {
        return std::nullopt;
    }

    const auto end = limit + length;
    const auto [parsedEnd, error] = std::from_chars(limit, end, config.maxPayloadBytes);
    if (error != std::errc{} || parsedEnd != end ||
        config.maxPayloadBytes < Protocol::FloatBytes || config.maxPayloadBytes > Protocol::MaxPayloadBytes) {
        return std::nullopt;
    }
    return config;
}
