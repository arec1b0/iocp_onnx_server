cmake_minimum_required(VERSION 3.20)
project(iocp_onnx_server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Required for Windows IOCP and Sockets
link_libraries(ws2_32 mswsock)

# Direct paths for manually extracted ONNX Runtime
set(ONNXRUNTIME_DIR "C:/onnxruntime" CACHE PATH "Extracted ONNX Runtime distribution")
include_directories(src ${ONNXRUNTIME_DIR}/include)

set(IOCP_SOURCES
    src/inference/InferenceProfile.cpp
    src/server/IocpServer.cpp
    src/server/Connection.cpp
    src/server/Protocol.cpp
    src/utils/Logger.cpp
    src/utils/Metrics.cpp
)

set(SOURCES
    src/main.cpp
    ${IOCP_SOURCES}
    src/inference/OnnxModel.cpp
    src/inference/TensorUtils.cpp
    src/utils/Config.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE onecore "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")

# Auto-copy all ONNX Runtime DLLs to the output directory
file(GLOB ORT_DLLS "${ONNXRUNTIME_DIR}/lib/*.dll")
foreach(DLL_FILE ${ORT_DLLS})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DLL_FILE}"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol_tests
        tests/ProtocolTests.cpp
        src/server/Connection.cpp
        src/server/Protocol.cpp
        src/inference/TensorUtils.cpp
    )
    add_executable(send_completion_tests
        tests/SendCompletionTests.cpp
        src/server/Connection.cpp
        src/server/Protocol.cpp
    )
    add_executable(iocp_lifecycle_tests
        tests/IocpLifecycleTests.cpp
        ${IOCP_SOURCES}
    )
    target_compile_definitions(iocp_lifecycle_tests PRIVATE IOCP_TEST_THREAD_FAILURE IOCP_TEST_SEND_SUBMISSION)
    add_executable(buffered_pipelining_tests
        tests/BufferedPipeliningTests.cpp
        ${IOCP_SOURCES}
    )
    add_executable(logger_tests tests/LoggerTests.cpp src/utils/Logger.cpp)
    target_compile_definitions(logger_tests PRIVATE IOCP_TEST_LOG_WRITES)
    target_link_libraries(iocp_lifecycle_tests PRIVATE onecore)
    target_link_libraries(buffered_pipelining_tests PRIVATE onecore)
    target_link_libraries(logger_tests PRIVATE onecore)
    set_target_properties(protocol_tests send_completion_tests iocp_lifecycle_tests buffered_pipelining_tests logger_tests
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests"
    )
    add_test(NAME protocol_framing COMMAND protocol_tests)
    add_test(NAME send_completion_state COMMAND send_completion_tests)
    add_test(NAME buffered_pipelining_state COMMAND buffered_pipelining_tests)
    add_test(NAME iocp_lifecycle COMMAND iocp_lifecycle_tests)
    add_test(NAME connection_policies COMMAND iocp_lifecycle_tests --connection-policies)
    add_test(NAME inference_scheduler COMMAND iocp_lifecycle_tests --inference-scheduler)
    add_test(NAME stdout_slow_reader COMMAND logger_tests slow)
    add_test(NAME stdout_broken_partial_record COMMAND logger_tests broken)
    add_test(NAME stdout_concurrent_pressure COMMAND logger_tests pressure)
    add_test(NAME stdout_loss_unreportable COMMAND logger_tests unavailable)
    set_tests_properties(iocp_lifecycle connection_policies inference_scheduler PROPERTIES LABELS network TIMEOUT 90)
    set_tests_properties(stdout_slow_reader stdout_broken_partial_record stdout_concurrent_pressure stdout_loss_unreportable
        PROPERTIES LABELS logging TIMEOUT 10)
endif()
