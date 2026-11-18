[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $ServerExe,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [ValidateRange(1, 10)][int] $Iterations = 3
)

$ErrorActionPreference = 'Stop'
$path = (Resolve-Path -LiteralPath $ServerExe).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh output directory.' }
$directory = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
Add-Type -Path (Join-Path $PSScriptRoot 'ReliabilityHarness.cs')
$env:IOCP_ONNX_IDLE_TIMEOUT_MS = '60000'
$env:IOCP_ONNX_FRAME_TIMEOUT_MS = '10000'
$env:IOCP_ONNX_MAX_PAYLOAD_BYTES = '40'
$env:IOCP_ONNX_MAX_CONNECTIONS = '16'
$env:IOCP_ONNX_TIMEOUT_CHECK_INTERVAL_MS = '25'
$budgetMs = 2000
$results = [Collections.Generic.List[object]]::new()

function Test-BlockedStdout([int] $Iteration) {
    $run = (New-Item -ItemType Directory -Path (Join-Path $directory "run-$Iteration")).FullName
    $model = (New-Item -ItemType Directory -Path (Join-Path $run 'models')).FullName
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../models/model.onnx') -Destination $model
    $stderr = Join-Path $run 'server.stderr.log'
    $pipe = [IO.Pipes.AnonymousPipeServerStream]::new([IO.Pipes.PipeDirection]::In,
        [IO.HandleInheritability]::Inheritable, 4096)
    $handle = $pipe.ClientSafePipeHandle.DangerousGetHandle()
    $reader = [IO.StreamReader]::new($pipe)
    $server = $null
    $client = $null
    $result = [ordered]@{ iteration = $Iteration; status = 'failed' }
    try {
        $server = [ReliabilityHarness]::StartServer($path, $run, $handle, $stderr)
        $startup = [Diagnostics.Stopwatch]::StartNew()
        do {
            $remaining = [TimeSpan]::FromSeconds(5) - $startup.Elapsed
            if ($remaining -le [TimeSpan]::Zero) { throw 'Server did not reach startup telemetry.' }
            $read = $reader.ReadLineAsync()
            if (-not $read.Wait($remaining) -or $null -eq $read.Result) { throw 'Server did not reach startup telemetry.' }
        } until ($read.Result.StartsWith('{"event":"connection_metrics"'))
        $initial = $read.Result | ConvertFrom-Json
        if ($initial.phase -ne 'running' -or $initial.connections_active -ne 0 -or $initial.pending_io_operations -ne 0) {
            throw 'Unexpected startup metrics.'
        }
        $client = [ReliabilityHarness]::OpenInferredClient(5000)
        $fill = [ReliabilityHarness]::FillStdoutPipe($pipe.SafePipeHandle.DangerousGetHandle(), $handle)
        if (-not $fill.Wait(2500)) { throw 'The attempt to fill stdout exceeded its deadline.' }
        $result.pipe_bytes_before_signal = $fill.Result

        # Leave the reader untouched from this point through process exit or deadline.
        $interrupt = [Diagnostics.Stopwatch]::StartNew()
        try { $result.exit_code = [ReliabilityHarness]::InterruptServer($server, $budgetMs) }
        finally { $result.shutdown_ms = $interrupt.Elapsed.TotalMilliseconds }
        if ($result.exit_code -ne 0 -or $result.shutdown_ms -gt $budgetMs) {
            throw 'The server did not exit successfully within the blocked-stdout shutdown budget.'
        }

        $records = @(Get-Content -LiteralPath $stderr | Where-Object { $_.StartsWith('{') } | ConvertFrom-Json)
        $final = @($records | Where-Object event -eq 'connection_metrics')
        $loss = @($records | Where-Object event -eq 'log_delivery')
        if ($final.Count -ne 1 -or $final[0].phase -ne 'stopped' -or $final[0].connections_active -ne 0 -or
            $final[0].pending_io_operations -ne 0 -or $final[0].connections_accepted_total -lt 1 -or
            $final[0].connections_accepted_total -ne $final[0].connections_closed_total -or
            $final[0].cancellation_requests_total -lt 1) {
            throw 'The fallback metrics did not confirm live connection and pending I/O drain.'
        }
        if ($loss.Count -ne 1 -or $loss[0].stdout_dropped -lt 1 -or -not $loss[0].final_snapshot_delivered) {
            throw 'Shutdown did not report stdout loss and successful delivery through stderr.'
        }
        $result.final_snapshot = $final[0]
        $result.loss_report = $loss[0]
        $result.status = 'passed'
    } catch {
        $result.error = $_.Exception.GetBaseException().Message
        throw
    } finally {
        if ($server) {
            if (-not $server.HasExited) { $server.Kill(); [void]$server.WaitForExit(5000) }
            $server.Dispose()
        }
        if ($client) { $client.Dispose() }
        $reader.Dispose()
        $pipe.Dispose()
        $results.Add($result)
    }
}

try {
    for ($i = 1; $i -le $Iterations; ++$i) { Test-BlockedStdout $i }
} finally {
    $summary = [ordered]@{ budget_ms = $budgetMs; trials = $results.ToArray() } | ConvertTo-Json -Depth 7
    $summary | Set-Content -LiteralPath (Join-Path $directory 'result.json')
    [Console]::WriteLine($summary)
}
