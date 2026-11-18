[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $OnnxRuntimeDir,
    [Parameter(Mandatory)][string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ort = (Resolve-Path -LiteralPath $OnnxRuntimeDir).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh diagnostics output directory.' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$archive = Join-Path $output 'source.zip'
$source = Join-Path $output 'source'
$build = Join-Path $output 'build'

# Build only an archived Stage 2.2 tree. The production source and targets remain separate.
git -C $repo archive --format=zip --output $archive becab75bb6cdfd4aef31c8206b1abaae875a69dc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Expand-Archive -LiteralPath $archive -DestinationPath $source
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'MetricsSinkProbe.cpp') -Destination (Join-Path $source 'src/utils/Metrics.cpp')
cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 "-DONNXRUNTIME_DIR=$ort"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build --config Release --target iocp_onnx_server --parallel 1 -- /nr:false
exit $LASTEXITCODE
