#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "server/ConnectionLimits.h"
#include "inference/InferenceLimits.h"

/// Runtime configuration for the server.
///
struct Config {
    std::string modelPath;   ///< Path to the ONNX model, relative to the CWD.
    std::string ipAddress;   ///< IPv4 listen address.
    int port;                ///< TCP listen port.
    int workerThreads;       ///< IOCP worker pool size.
    std::uint32_t maxPayloadBytes;  ///< Limit for each float32 payload.
    ConnectionLimits connectionLimits;
    InferenceLimits inferenceLimits;
    bool connectionTrace;
    bool inferenceProfile;

    /// Returns the default configuration: models/model.onnx, 0.0.0.0:8080,
    /// worker count = hardware_concurrency (fallback 2), a 16 MiB
    /// payload limit, and default connection limits. Invalid overrides
    /// are logged and cause loading to fail.
    static std::optional<Config> Load();
};
