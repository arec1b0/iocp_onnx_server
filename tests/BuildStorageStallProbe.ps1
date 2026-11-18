#Requires -Version 7.0
param(
    [Parameter(Mandatory)] [string] $OnnxRuntimeDir,
    [Parameter(Mandatory)] [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ort = (Resolve-Path -LiteralPath $OnnxRuntimeDir).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh diagnostics output directory.' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$source = (New-Item -ItemType Directory -Path (Join-Path $output 'source')).FullName
$build = Join-Path $output 'build'
Copy-Item -LiteralPath (Join-Path $repo 'src') -Destination $source -Recurse
Copy-Item -LiteralPath (Join-Path $repo 'CMakeLists.txt') -Destination $source
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'StorageStallHooks.h') -Destination (Join-Path $source 'src/utils')

# Interpose only the stream operations in the copied working tree.
$file = Join-Path $source 'src/utils/Metrics.cpp'
$contents = Get-Content -LiteralPath $file -Raw
$replacements = [ordered]@{
    '#include "Logger.h"' = "#include `"Logger.h`"`n#include `"StorageStallHooks.h`""
    'logFile_.write(record.data(), static_cast<std::streamsize>(length))' =
        'StorageProbe::Append(logFile_, record.data(), static_cast<std::streamsize>(length))'
    'logFile_.flush()' = 'StorageProbe::Flush(logFile_)'
    'logFile_.close();' = 'StorageProbe::Close(logFile_);'
}
foreach ($anchor in $replacements.Keys) {
    if ([regex]::Matches($contents, [regex]::Escape($anchor)).Count -ne 1) { throw "Unexpected metrics source near $anchor." }
    $contents = $contents.Replace($anchor, $replacements[$anchor])
}
Set-Content -LiteralPath $file -Value $contents -NoNewline
cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 "-DONNXRUNTIME_DIR=$ort" -DBUILD_TESTING=OFF
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build --config Release --target iocp_onnx_server --parallel 2 -- /nr:false
exit $LASTEXITCODE
