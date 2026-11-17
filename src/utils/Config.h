#pragma once
#include <cstdint>
#include <optional>
#include <string>

/// Runtime configuration for the server.
///
struct Config {
    std::string modelPath;   ///< Path to the ONNX model, relative to the CWD.
    std::string ipAddress;   ///< IPv4 listen address.
    int port;                ///< TCP listen port.
    int workerThreads;       ///< IOCP worker pool size.
    std::uint32_t maxPayloadBytes;  ///< Limit for each float32 payload.

    /// Returns the default configuration: models/model.onnx, 0.0.0.0:8080,
    /// worker count = hardware_concurrency (fallback 2), and a 16 MiB
    /// payload limit. Returns no value for an invalid environment override.
    static std::optional<Config> Load();
};
