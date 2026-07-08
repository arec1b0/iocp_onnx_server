#include "OnnxModel.h"
#include "TensorUtils.h"
#include "utils/Logger.h"

OnnxModel::OnnxModel() 
    : env(ORT_LOGGING_LEVEL_WARNING, "IOCP_ONNX_Server"),
      session(nullptr),
      memoryInfo(Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU)) {}

bool OnnxModel::Load(const std::string& modelPath) {
    try {
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Convert string path to wstring for Windows API compatibility
        std::wstring wModelPath(modelPath.begin(), modelPath.end());
        session = Ort::Session(env, wModelPath.c_str(), sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;

        // Retrieve metadata for the first input node
        size_t numInputNodes = session.GetInputCount();
        if (numInputNodes > 0) {
            Ort::AllocatedStringPtr inputNamePtr = session.GetInputNameAllocated(0, allocator);
            inputName = inputNamePtr.get();
            
            Ort::TypeInfo typeInfo = session.GetInputTypeInfo(0);
            auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
            inputShape = tensorInfo.GetShape();
            
            // Handle dynamic batch sizes by setting a static fallback layout
            for (size_t i = 0; i < inputShape.size(); ++i) {
                if (inputShape[i] < 0) {
                    inputShape[i] = 1;
                }
            }
        }

        // Retrieve metadata for the first output node
        size_t numOutputNodes = session.GetOutputCount();
        if (numOutputNodes > 0) {
            Ort::AllocatedStringPtr outputNamePtr = session.GetOutputNameAllocated(0, allocator);
            outputName = outputNamePtr.get();

            Ort::TypeInfo typeInfo = session.GetOutputTypeInfo(0);
            auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
            outputShape = tensorInfo.GetShape();
            
            for (size_t i = 0; i < outputShape.size(); ++i) {
                if (outputShape[i] < 0) {
                    outputShape[i] = 1;
                }
            }
        }

        LOG_INFO("Model loaded successfully from: " + modelPath);
        return true;
    }
    catch (const Ort::Exception& e) {
        LOG_ERR("ONNX Runtime Exception during model load: " + std::string(e.what()));
        return false;
    }
}

std::vector<float> OnnxModel::RunInference(const std::vector<float>& inputData) {
    std::vector<float> outputData;
    try {
        size_t inputElementCount = TensorUtils::GetTensorElementsCount(inputShape);
        if (inputData.size() != inputElementCount) {
            LOG_ERR("Input data size mismatch. Expected: " + std::to_string(inputElementCount) + ", Got: " + std::to_string(inputData.size()));
            return outputData;
        }

        size_t outputElementCount = TensorUtils::GetTensorElementsCount(outputShape);
        outputData.resize(outputElementCount);

        // Bind raw continuous memory directly to prevent extra tensor allocation overhead
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memoryInfo,
            const_cast<float*>(inputData.data()),
            inputData.size(),
            inputShape.data(),
            inputShape.size()
        );

        Ort::Value outputTensor = Ort::Value::CreateTensor<float>(
            memoryInfo,
            outputData.data(),
            outputData.size(),
            outputShape.data(),
            outputShape.size()
        );

        const char* inputNames[] = { inputName.c_str() };
        const char* outputNames[] = { outputName.c_str() };

        session.Run(
            Ort::RunOptions{nullptr},
            inputNames,
            &inputTensor,
            1,
            outputNames,
            &outputTensor,
            1
        );
    }
    catch (const Ort::Exception& e) {
        LOG_ERR("ONNX Runtime Exception during execution: " + std::string(e.what()));
        outputData.clear();
    }
    return outputData;
}