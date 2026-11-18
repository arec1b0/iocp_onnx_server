[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $PackageFile
)

$ErrorActionPreference = 'Stop'
$version = '1.18.1'
$package = "microsoft.ml.onnxruntime.$version.nupkg"
$expectedHash = 'E1FDEB5359407C948569905B4EB76AF50B8BDB38B2BD3F2AD6516FE4A7186D6A'
$outputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $outputPath) { throw 'Choose a fresh ONNX Runtime output directory.' }

if ($PackageFile) {
    $archive = (Resolve-Path -LiteralPath $PackageFile).Path
} else {
    $parent = Split-Path -Parent $outputPath
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $archive = Join-Path $parent $package
    $uri = "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$version/$package"
    Write-Output "Downloading Microsoft.ML.OnnxRuntime $version from $uri"
    Invoke-WebRequest -Uri $uri -OutFile $archive -TimeoutSec 120
}

# The pinned bytes were verified against the Microsoft author and NuGet repository signatures.
$actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
if ($actualHash -ne $expectedHash) {
    throw "ONNX Runtime package SHA-256 mismatch. Expected $expectedHash, got $actualHash."
}
Write-Output "Verified ONNX Runtime $version SHA-256 $actualHash"

$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $files = [ordered]@{
        'runtimes/win-x64/native/onnxruntime.lib' = 'lib/onnxruntime.lib'
        'runtimes/win-x64/native/onnxruntime.dll' = 'lib/onnxruntime.dll'
        'runtimes/win-x64/native/onnxruntime_providers_shared.dll' = 'lib/onnxruntime_providers_shared.dll'
        'LICENSE' = 'LICENSE'
        'ThirdPartyNotices.txt' = 'ThirdPartyNotices.txt'
    }
    foreach ($entry in $zip.Entries) {
        if ($entry.FullName -cmatch '^build/native/include/([A-Za-z0-9_]+\.h)$') {
            $files[$entry.FullName] = "include/$($Matches[1])"
        }
    }
    if (-not $files.Contains('build/native/include/onnxruntime_cxx_api.h')) {
        throw 'The ONNX Runtime package lacks the C++ API headers.'
    }
    foreach ($source in $files.Keys) {
        if (-not $zip.GetEntry($source)) { throw "The ONNX Runtime package lacks $source." }
    }

    New-Item -ItemType Directory -Force -Path (Join-Path $outputPath 'include'), (Join-Path $outputPath 'lib') | Out-Null
    foreach ($source in $files.Keys) {
        [IO.Compression.ZipFileExtensions]::ExtractToFile($zip.GetEntry($source), (Join-Path $outputPath $files[$source]))
    }
} finally {
    $zip.Dispose()
}
Write-Output "Prepared x64 ONNX Runtime $version in $outputPath"
