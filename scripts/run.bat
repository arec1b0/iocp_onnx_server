@echo off
setlocal

set "EXE_PATH=%~dp0..\build\Release\iocp_onnx_server.exe"

if exist "%EXE_PATH%" (
    echo [INFO] Starting IOCP ONNX Server...
    "%EXE_PATH%"
) else (
    echo [ERROR] Executable not found. Run scripts\build.bat first.
    exit /b 1
)

endlocal