@echo off
setlocal

echo [INFO] Creating build directory...
if not exist "%~dp0..\build" mkdir "%~dp0..\build"
cd /d "%~dp0..\build"

echo [INFO] Running CMake configuration...
cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release ..

echo [INFO] Building the project...
cmake --build . --config Release

endlocal