cmake_minimum_required(VERSION 3.20)
project(iocp_onnx_server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Required for Windows IOCP and Sockets
link_libraries(ws2_32 mswsock)

# Direct paths for manually extracted ONNX Runtime
set(ONNXRUNTIME_DIR "C:/onnxruntime")
include_directories(src ${ONNXRUNTIME_DIR}/include)

set(SOURCES
    src/main.cpp
    src/server/IocpServer.cpp
    src/server/Connection.cpp
    src/inference/OnnxModel.cpp
    src/inference/TensorUtils.cpp
    src/utils/Logger.cpp
    src/utils/Config.cpp
    src/utils/Metrics.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")

# Auto-copy all ONNX Runtime DLLs to the output directory
file(GLOB ORT_DLLS "${ONNXRUNTIME_DIR}/lib/*.dll")
foreach(DLL_FILE ${ORT_DLLS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DLL_FILE}"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endforeach()