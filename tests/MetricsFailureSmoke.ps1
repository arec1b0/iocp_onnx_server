[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $ServerExe,
    [Parameter(Mandatory)][string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
$executable = (Resolve-Path -LiteralPath $ServerExe).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh output directory.' }
$root = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
Add-Type -Path (Join-Path $PSScriptRoot 'ReliabilityHarness.cs')
$env:IOCP_ONNX_MAX_PAYLOAD_BYTES = '40'
$results = foreach ($case in 'directory', 'partial-tail') {
    $directory = (New-Item -ItemType Directory -Path (Join-Path $root $case)).FullName
    $models = (New-Item -ItemType Directory -Path (Join-Path $directory 'models')).FullName
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../models/model.onnx') -Destination (Join-Path $models 'model.onnx')
    $metrics = Join-Path $directory 'metrics.jsonl'
    $tail = '{"timestamp":'
    if ($case -eq 'directory') { $null = New-Item -ItemType Directory -Path $metrics }
    else { [IO.File]::WriteAllText($metrics, $tail) }

    $stdout = Join-Path $directory 'stdout.log'
    $stderr = Join-Path $directory 'stderr.log'
    $server = [ReliabilityHarness]::StartServer($executable, $directory, $stdout, $stderr)
    try {
        $first = [ReliabilityHarness]::OpenInferredClient(5000)
        $first.Dispose()
        $second = [ReliabilityHarness]::OpenInferredClient(5000)
        $second.Dispose()
        $exitCode = [ReliabilityHarness]::InterruptServer($server, 5000)
        if ($exitCode -ne 4) { throw "Expected metrics failure exit status 4, got $exitCode." }

        $prefix = '{"event":"inference_metrics_delivery_failed"'
        $reports = @(Get-Content -LiteralPath $stderr | Where-Object { $_.Contains($prefix) } |
            ForEach-Object { $_.Substring($_.IndexOf($prefix)) | ConvertFrom-Json })
        if ($reports.Count -ne 1 -or -not $reports[0].open_failed -or $reports[0].skipped -ne 2 -or
            $reports[0].appended -ne 0 -or $reports[0].flushed -ne 0) { throw 'Metrics failure accounting did not reconcile.' }
        if ($case -eq 'partial-tail' -and [IO.File]::ReadAllText($metrics) -cne $tail) { throw 'The existing partial tail changed.' }

        $drain = Get-Content -LiteralPath $stdout | Where-Object { $_.StartsWith('{"event":"connection_metrics"') } |
            ForEach-Object { ConvertFrom-Json $_ } | Where-Object phase -eq 'stopped'
        if (-not $drain -or $drain.connections_active -ne 0 -or $drain.pending_io_operations -ne 0) { throw 'Network shutdown did not drain.' }
        [ordered]@{case=$case; exact_responses=2; exit_code=$exitCode; delivery=$reports[0]; drained=$true}
    } finally {
        if (-not $server.HasExited) { $server.Kill(); $null = $server.WaitForExit(5000) }
        $server.Dispose()
    }
}
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $root 'result.json')
