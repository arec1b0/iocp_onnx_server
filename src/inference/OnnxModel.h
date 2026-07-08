#pragma once
#include <string>
#include <vector>
#include <onnxruntime_cxx_api.h>

/// Thin wrapper around an ONNX Runtime session serving a single model.
///
/// Only the model's first input and first output node are used; both are
/// assumed to be float32 tensors. Dynamic dimensions (-1) are pinned to 1 at
/// load time, so the effective shapes — and therefore the element counts the
/// wire protocol expects — are static for the lifetime of the process.
///
/// RunInference is called concurrently from IOCP worker threads;
/// Ort::Session::Run is thread-safe, and intra-op parallelism is set to 1 so
/// that throughput scales with concurrent requests rather than per-request
/// threading.
class OnnxModel {
public:
    OnnxModel();
    ~OnnxModel() = default;

    /// Creates the session and caches input/output names and shapes.
    /// @param modelPath  Path to the .onnx file (narrow string; converted to
    ///                   wide for the Windows ORT API).
    /// @return false if the model could not be loaded (logged).
    bool Load(const std::string& modelPath);

    /// Runs one inference, binding the input and a pre-sized output buffer
    /// directly as ORT tensors (no ORT-managed allocations on the hot path).
    /// @param inputData  Flat input tensor; its size must equal the product
    ///                   of the model's input dimensions.
    /// @return Flat output tensor, or an empty vector on size mismatch or
    ///         runtime failure (logged).
    std::vector<float> RunInference(const std::vector<float>& inputData);

private:
    Ort::Env env;
    Ort::Session session;
    Ort::MemoryInfo memoryInfo;

    std::vector<int64_t> inputShape;   ///< First input's shape, dynamic dims pinned to 1.
    std::vector<int64_t> outputShape;  ///< First output's shape, dynamic dims pinned to 1.

    std::string inputName;
    std::string outputName;
};
