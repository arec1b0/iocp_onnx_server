#include "Config.h"
#include "Logger.h"
#include "server/Protocol.h"
#include <windows.h>
#include <charconv>
#include <thread>

namespace {
    std::optional<std::uint32_t> ReadOverride(const char* name, std::uint32_t fallback,
                                              std::uint32_t minimum, std::uint32_t maximum) {
        char digits[11];
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableA(name, digits, sizeof(digits));
        if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return fallback;
        }
        if (length != 0 && length < sizeof(digits)) {
            std::uint32_t value = 0;
            const auto end = digits + length;
            const auto [parsedEnd, error] = std::from_chars(digits, end, value);
            if (error == std::errc{} && parsedEnd == end && value >= minimum && value <= maximum) {
                return value;
            }
        }
        LOG_ERR(std::string(name) + " must be a decimal integer from " +
                std::to_string(minimum) + " to " + std::to_string(maximum) + ".");
        return std::nullopt;
    }
}

std::optional<Config> Config::Load() {
    Config config;
    config.modelPath = "models/model.onnx";
    config.ipAddress = "0.0.0.0";
    config.port = 8080;
    
    unsigned int hardware_threads = std::thread::hardware_concurrency();
    config.workerThreads = hardware_threads > 0 ? hardware_threads : 2;

    const auto payload = ReadOverride("IOCP_ONNX_MAX_PAYLOAD_BYTES", Protocol::DefaultMaxPayloadBytes,
                                      Protocol::FloatBytes, Protocol::MaxPayloadBytes);
    const ConnectionLimits defaults;
    const auto connections = ReadOverride("IOCP_ONNX_MAX_CONNECTIONS", defaults.maxConnections,
                                          1, ConnectionLimits::MaxConnectionCount);
    const auto idle = ReadOverride("IOCP_ONNX_IDLE_TIMEOUT_MS", static_cast<std::uint32_t>(defaults.idleTimeout.count()),
                                   1, ConnectionLimits::MaxTimeoutMs);
    const auto frame = ReadOverride("IOCP_ONNX_FRAME_TIMEOUT_MS", static_cast<std::uint32_t>(defaults.frameTimeout.count()),
                                    1, ConnectionLimits::MaxTimeoutMs);
    const auto interval = ReadOverride("IOCP_ONNX_TIMEOUT_CHECK_INTERVAL_MS",
                                       static_cast<std::uint32_t>(defaults.checkInterval.count()),
                                       1, ConnectionLimits::MaxCheckIntervalMs);
    const auto trace = ReadOverride("IOCP_ONNX_CONNECTION_TRACE", 0, 0, 1);
    if (!payload || !connections || !idle || !frame || !interval || !trace) {
        return std::nullopt;
    }
    using std::chrono::milliseconds;
    config.maxPayloadBytes = *payload;
    config.connectionLimits = {*connections, milliseconds(*idle), milliseconds(*frame), milliseconds(*interval)};
    config.connectionTrace = *trace != 0;
    return config;
}
