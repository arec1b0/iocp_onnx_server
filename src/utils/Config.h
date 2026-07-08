#pragma once
#include <string>

/// Runtime configuration for the server.
///
/// Currently compiled-in defaults only (no CLI flags, env vars, or config
/// file) — see Config::Load for the values.
struct Config {
    std::string modelPath;   ///< Path to the ONNX model, relative to the CWD.
    std::string ipAddress;   ///< IPv4 listen address.
    int port;                ///< TCP listen port.
    int workerThreads;       ///< IOCP worker pool size.

    /// Returns the default configuration: models/model.onnx, 0.0.0.0:8080,
    /// worker count = hardware_concurrency (fallback 2).
    static Config Load();
};
