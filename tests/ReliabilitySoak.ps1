[CmdletBinding()]
param(
    [Parameter(ParameterSetName = 'Run', Mandatory)][string] $ServerExe,
    [Parameter(ParameterSetName = 'Run', Mandatory)][string] $OutputDirectory,
    [Parameter(ParameterSetName = 'Run')][ValidateRange(1, 86400)][int] $DurationSeconds = 3600,
    [Parameter(ParameterSetName = 'Run')][switch] $Smoke,
    [Parameter(ParameterSetName = 'Run')][int] $Seed = 17381,
    [Parameter(ParameterSetName = 'Run')][hashtable] $Mix = @{
        persistent = 2; short = 1; fragmented = 1; idle = 1
        slow = 1; stalled = 1; reset = 1; saturation = 1
    },
    [Parameter(ParameterSetName = 'Run')][ValidateRange(8, 64)][int] $Capacity = 16,
    [Parameter(ParameterSetName = 'Run')][ValidateRange(32768, 262144)][int] $OutputRows = 131072,
    [Parameter(ParameterSetName = 'Run')][ValidateRange(1, 400)][int] $RequestGapMs = 180,
    [Parameter(ParameterSetName = 'Run')][ValidateRange(1000, 60000)][int] $SaturationIntervalMs = 12000,
    [Parameter(ParameterSetName = 'Run')][ValidateRange(4000, 20000)][int] $OperationTimeoutMs = 8000,
    [Parameter(ParameterSetName = 'Run')][switch] $ConnectionDiagnostics,
    [Parameter(ParameterSetName = 'Run')][ValidateCount(1, 3)][ValidateRange(5, 40)][int[]] $ConnectionRates,
    [Parameter(ParameterSetName = 'Run')][switch] $ReuseConnections,
    [Parameter(ParameterSetName = 'Run')][switch] $ProbePolicies,
    [Parameter(ParameterSetName = 'Run')][string] $OverlapBuild,
    [Parameter(ParameterSetName = 'Run')][ValidateRange(1, 65535)][int] $DynamicPortStart = 49152,
    [Parameter(ParameterSetName = 'Run')][ValidateRange(1, 65535)][int] $DynamicPortCount = 16384,
    [Parameter(ParameterSetName = 'Controller', Mandatory)][string] $ControllerConfig
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

if ($PSCmdlet.ParameterSetName -eq 'Run') {
    if ($Smoke -and -not $PSBoundParameters.ContainsKey('DurationSeconds')) { $DurationSeconds = 45 }
    $controlled = $ConnectionRates.Count -gt 0 -or $ProbePolicies
    if (($controlled -or $OverlapBuild) -and -not $ConnectionDiagnostics) { throw 'Use -ConnectionDiagnostics for controlled experiments.' }
    if ($controlled -and $OverlapBuild -or $ConnectionRates.Count -gt 0 -and $ProbePolicies -or
        $ReuseConnections -and $ConnectionRates.Count -eq 0 -or $DynamicPortStart + $DynamicPortCount -gt 65536) {
        throw 'Invalid combination of connection experiment settings.'
    }
    $configuredRows = if ($controlled) { 1 } else { $OutputRows }
    if ($controlled) { $DurationSeconds = 90 }
    if ($OverlapBuild) {
        $OverlapBuild = (Resolve-Path -LiteralPath $OverlapBuild).Path
        if ($DurationSeconds -gt 90) { throw 'The overlapping experiment is limited to 90 seconds.' }
    }
    $known = @('persistent', 'short', 'fragmented', 'idle', 'slow', 'stalled', 'reset', 'saturation')
    foreach ($role in $Mix.Keys) {
        if ($role -notin $known -or $Mix[$role] -isnot [int] -or $Mix[$role] -lt 0 -or $Mix[$role] -gt 32) {
            throw "Invalid workload role or worker count for $role."
        }
    }
    $roles = @(foreach ($role in $known) { for ($i = 0; $i -lt $Mix[$role]; ++$i) { $role } })
    if (-not $controlled -and ($roles.Count -eq 0 -or $roles.Count - $Mix['saturation'] -gt $Capacity - 4)) {
        throw 'Use at least one worker and leave four admission slots free for normal workloads.'
    }
    $path = (Resolve-Path -LiteralPath $ServerExe).Path
    if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh output directory.' }
    $directory = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
    $configPath = Join-Path $directory 'workload.json'
    [ordered]@{
        executable = $path; directory = $directory; duration_seconds = $DurationSeconds
        seed = $Seed; roles = $roles; capacity = $Capacity; output_rows = $configuredRows
        gap_ms = $RequestGapMs; saturation_interval_ms = $SaturationIntervalMs
        operation_timeout_ms = $OperationTimeoutMs; idle_ms = 1200; frame_ms = 600; check_ms = 25
        diagnostics = [bool]$ConnectionDiagnostics; rates = @($ConnectionRates); reuse = [bool]$ReuseConnections
        policy_probes = [bool]$ProbePolicies; controlled = [bool]$controlled; overlap_build = $OverlapBuild
        dynamic_port_start = $DynamicPortStart; dynamic_port_count = $DynamicPortCount
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $configPath
    Write-Host "Reliability evidence will be written to $directory"

    # The isolated controller has no caller console to detach from.
    $start = [Diagnostics.ProcessStartInfo]::new((Get-Process -Id $PID).Path)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($arg in @('-NoProfile', '-NonInteractive', '-File', $PSCommandPath, '-ControllerConfig', $configPath)) {
        $start.ArgumentList.Add($arg)
    }
    $controller = [Diagnostics.Process]::Start($start)
    try {
        $output = $controller.StandardOutput.ReadToEndAsync()
        $errors = $controller.StandardError.ReadToEndAsync()
        if (-not $controller.WaitForExit(($DurationSeconds + 120) * 1000)) {
            $controller.Kill($true)
            throw 'The isolated reliability controller exceeded its overall deadline.'
        }
        $output.GetAwaiter().GetResult()
        if ($controller.ExitCode -ne 0) { throw $errors.GetAwaiter().GetResult() }
    } finally {
        $controller.Dispose()
    }
    return
}

Add-Type -Path (Join-Path $PSScriptRoot 'ReliabilityHarness.cs')
$config = Get-Content -LiteralPath $ControllerConfig -Raw | ConvertFrom-Json -AsHashtable
$directory = $config.directory
$stdout = Join-Path $directory 'server.stdout.log'
$stderr = Join-Path $directory 'server.stderr.log'
$snapshots = Join-Path $directory 'connection-snapshots.jsonl'
$resources = Join-Path $directory 'process-samples.jsonl'
$resultPath = Join-Path $directory 'result.json'
$fields = @(
    'connections_accepted_total', 'connections_rejected_capacity_total', 'connections_closed_total',
    'connections_active', 'connections_high_watermark', 'idle_timeouts_total', 'incomplete_frame_timeouts_total',
    'send_timeouts_total', 'io_failures_total', 'pending_io_operations',
    'cancellation_requests_total', 'cancellation_completions_total'
)
$history = [System.Collections.Generic.List[object]]::new()
$resourceHistory = [System.Collections.Generic.List[object]]::new()
$lineOffset = 0
$server = $null
$workload = $null
$overlap = $null
$runClock = [Diagnostics.Stopwatch]::new()
$result = [ordered]@{ status = 'failed'; workload = $config }

function Read-Telemetry {
    if (-not (Test-Path -LiteralPath $stdout)) { return }
    $stream = [IO.File]::Open($stdout, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = [IO.StreamReader]::new($stream)
    try { $lines = @($reader.ReadToEnd() -split '\r?\n') }
    finally { $reader.Dispose() }
    for (; $script:lineOffset -lt $lines.Count - 1; $script:lineOffset++) {
        $line = $lines[$script:lineOffset]
        if (-not $line.StartsWith('{')) { continue }
        $item = $line | ConvertFrom-Json -AsHashtable
        if ($item.event -ne 'connection_metrics' -or $item.phase -notin @('running', 'stopped') -or
            $item.timestamp_ms -le 0 -or $item.Keys.Count -ne ($fields.Count + 3)) {
            throw 'Unexpected operational JSON schema.'
        }
        foreach ($field in $fields) {
            if (-not $item.Contains($field) -or ($item[$field] -isnot [long] -and $item[$field] -isnot [int]) -or
                $item[$field] -lt 0) { throw "Invalid operational metric $field." }
            if ($history.Count -gt 0 -and $field -notin @('connections_active', 'pending_io_operations') -and
                $item[$field] -lt $history[$history.Count - 1][$field]) { throw "Counter regressed for $field." }
        }
        if ($item.connections_accepted_total - $item.connections_closed_total -ne $item.connections_active -or
            $item.pending_io_operations -gt $item.connections_active -or
            $item.connections_active -gt $item.connections_high_watermark -or
            $item.connections_high_watermark -gt $config.capacity) {
            throw 'Inconsistent connection metrics snapshot.'
        }
        $history.Add($item)
        $line | Add-Content -LiteralPath $snapshots
    }
}

function Sample-Resources([Diagnostics.Process] $Process) {
    $Process.Refresh()
    $sample = [ordered]@{
        elapsed_seconds = [Math]::Round($runClock.Elapsed.TotalSeconds, 3)
        private_bytes = $Process.PrivateMemorySize64
        handles = $Process.HandleCount
        cpu_seconds = $Process.TotalProcessorTime.TotalSeconds
    }
    if ($config.diagnostics) {
        $sample.timestamp_us = [ReliabilityHarness]::UtcMicroseconds()
        $sample.tcp = [ReliabilityHarness]::TcpStates($config.dynamic_port_start, $config.dynamic_port_count)
        $sample.client_attempts = $workload.Connections.Issued
        $sample.client_closures = $workload.Connections.Recorded
        $sample.verified_responses = $workload.Requests
    }
    $resourceHistory.Add($sample)
    $sample | ConvertTo-Json -Compress | Add-Content -LiteralPath $resources
}

function Get-LatencyPercentile([long[]] $Counts, [long] $Total, [double] $Percentile) {
    if ($Total -eq 0) { return $null }
    $target = [Math]::Ceiling($Total * $Percentile)
    $seen = 0L
    for ($bin = 0; $bin -lt $Counts.Length; ++$bin) {
        $seen += $Counts[$bin]
        if ($seen -ge $target) { return $bin }
    }
    throw 'Latency histogram count does not match validated requests.'
}

try {
    $model = (New-Item -ItemType Directory -Path (Join-Path $directory 'models')).FullName
    if ($config.controlled) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../models/model.onnx') -Destination $model
    } else {
        [ReliabilityHarness]::WriteSoakModel((Join-Path $model 'model.onnx'), $config.output_rows)
    }
    $environment = @{
        IOCP_ONNX_MAX_PAYLOAD_BYTES = [string]($config.output_rows * 40)
        IOCP_ONNX_MAX_CONNECTIONS = [string]$config.capacity
        IOCP_ONNX_IDLE_TIMEOUT_MS = [string]$config.idle_ms
        IOCP_ONNX_FRAME_TIMEOUT_MS = [string]$config.frame_ms
        IOCP_ONNX_TIMEOUT_CHECK_INTERVAL_MS = [string]$config.check_ms
    }
    if ($config.diagnostics) { $environment.IOCP_ONNX_CONNECTION_TRACE = '1' }
    $saved = @{}
    try {
        foreach ($key in $environment.Keys) {
            $saved[$key] = [Environment]::GetEnvironmentVariable($key)
            [Environment]::SetEnvironmentVariable($key, $environment[$key])
        }
        $server = [ReliabilityHarness]::StartServer($config.executable, $directory, $stdout, $stderr)
    } finally {
        foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key]) }
    }

    $startup = [Diagnostics.Stopwatch]::StartNew()
    while ($history.Count -eq 0) {
        if ($server.HasExited -or $startup.Elapsed.TotalSeconds -gt 10) {
            throw 'The server did not reach startup telemetry.'
        }
        Read-Telemetry
        if ($history.Count -eq 0) { Start-Sleep -Milliseconds 20 }
    }
    if ($history[0].phase -ne 'running' -or $history[0].connections_accepted_total -ne 0) {
        throw 'Invalid startup telemetry.'
    }
    $workload = [ReliabilityHarness+Workload]::new($config.output_rows, $config.capacity,
        $config.operation_timeout_ms, $config.idle_ms, $config.frame_ms, $config.gap_ms, $config.saturation_interval_ms,
        [bool]$config.diagnostics)
    if ($config.overlap_build) {
        $launch = [Diagnostics.ProcessStartInfo]::new((Get-Command ctest -CommandType Application).Source)
        $launch.UseShellExecute = $false
        $launch.CreateNoWindow = $true
        $launch.RedirectStandardOutput = $true
        $launch.RedirectStandardError = $true
        foreach ($arg in @('--test-dir', $config.overlap_build, '-C', 'Release', '-R', '^iocp_lifecycle$',
                '--repeat', 'until-fail:3', '-V')) { $launch.ArgumentList.Add($arg) }
        $overlap = [Diagnostics.Process]::Start($launch)
        $result.overlap_started_us = [ReliabilityHarness]::UtcMicroseconds()
        $overlapOutput = $overlap.StandardOutput.ReadToEndAsync()
        $overlapErrors = $overlap.StandardError.ReadToEndAsync()
    }
    $runClock.Start()
    $task = if ($config.controlled) {
        $workload.RunControlled([int[]]$config.rates, $config.reuse, $config.policy_probes, $config.seed)
    } else { $workload.Run([string[]]$config.roles, $config.seed, $config.duration_seconds) }
    $nextSample = 0
    do {
        if ($server.HasExited) { throw "Server exited during the soak with code $($server.ExitCode)." }
        if ($runClock.Elapsed.TotalSeconds -gt $config.duration_seconds + 30) { throw 'Workload deadline exceeded.' }
        Read-Telemetry
        if ($runClock.Elapsed.TotalSeconds -ge $nextSample) {
            Sample-Resources $server
            $nextSample = $runClock.Elapsed.TotalSeconds + $(if ($config.diagnostics) { 1 } else { 5 })
        }
    } while (-not $task.Wait(500))
    $runClock.Stop()
    if ($workload.Errors.Length) { throw ($workload.Errors -join ' | ') }
    if (-not $config.controlled -and $runClock.Elapsed.TotalSeconds -lt $config.duration_seconds) {
        throw 'Soak ended before the requested duration.'
    }
    if ($config.diagnostics) { Sample-Resources $server }
    Read-Telemetry
    $result.running_snapshots = $history.Count

    # Keep real sockets alive across the console signal. The final telemetry,
    # rather than peer closure alone, proves registry and IOCP drain.
    $shutdownClients = if (-not $config.controlled) {
        [ReliabilityHarness]::OpenSaturation($config.capacity, $config.operation_timeout_ms)
    }
    try {
        $shutdown = [Diagnostics.Stopwatch]::StartNew()
        $exitCode = [ReliabilityHarness]::InterruptServer($server, 15000)
        $result.shutdown_ms = $shutdown.Elapsed.TotalMilliseconds
    } finally {
        foreach ($client in $shutdownClients) { $client.Dispose() }
    }
    if ($exitCode -ne 0) { throw "Graceful server exit returned $exitCode." }
    Read-Telemetry
    $final = $history[$history.Count - 1]
    $result.final_snapshot = $final
    if ($final.phase -ne 'stopped' -or $final.connections_active -ne 0 -or $final.pending_io_operations -ne 0) {
        throw 'Missing a drained shutdown snapshot.'
    }
    if (-not $config.controlled -and $config.duration_seconds -ge 35 -and $result.running_snapshots -lt 2) {
        throw 'Periodic connection metrics were not observed.'
    }
    if ($config.controlled) {
        $expected = if ($config.policy_probes) { 2 * $config.capacity } else { 100 * $config.rates.Count }
        if ($workload.Requests -ne $expected) { throw "Expected $expected exact replies." }
        if ($config.policy_probes -and ($final.connections_rejected_capacity_total -ne 1 -or
                $final.idle_timeouts_total -ne 1 -or $final.incomplete_frame_timeouts_total -ne 1)) {
            throw 'Capacity, idle, or frame probe produced unexpected metrics.'
        }
    }
    foreach ($role in @($config.roles | Where-Object { -not $config.controlled } | Sort-Object -Unique)) {
        if (-not $workload.Episodes.ContainsKey($role)) { throw "No completed episode for $role." }
    }
    foreach ($pair in @(
        @('idle', 'idle_timeouts_total'), @('slow', 'incomplete_frame_timeouts_total'),
        @('stalled', 'send_timeouts_total'), @('saturation', 'connections_rejected_capacity_total')
    )) {
        if (-not $config.controlled -and $pair[0] -in $config.roles -and $final[$pair[1]] -eq 0) { throw "No evidence for $($pair[0]) policy." }
    }
    if (@($config.roles | Where-Object { $_ -in @('persistent', 'short', 'fragmented') }).Count -gt 0 -and
        $workload.Requests -eq 0) { throw 'No inference response was validated.' }
    $result.status = 'passed'
} catch {
    $result.error = $_.Exception.Message
    if ($workload) { $workload.Stop() }
    if ($server -and -not $server.HasExited) {
        try {
            $result.failure_shutdown_exit_code = [ReliabilityHarness]::InterruptServer($server, 15000)
            Read-Telemetry
            if ($history.Count -gt 0) { $result.failure_last_snapshot = $history[$history.Count - 1] }
        } catch {
            $result.failure_shutdown_error = $_.Exception.Message
        }
    }
    throw
} finally {
    if ($workload) { $workload.Stop() }
    if ($server) {
        if (-not $server.HasExited) { $server.Kill(); [void]$server.WaitForExit(5000) }
        $server.Dispose()
    }
    if ($overlap) {
        if (-not $overlap.WaitForExit(15000)) { $overlap.Kill($true); [void]$overlap.WaitForExit(5000) }
        $result.overlap_exit_code = $overlap.ExitCode
        $overlapOutput.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $directory 'overlap.stdout.log')
        $overlapErrors.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $directory 'overlap.stderr.log')
        $overlap.Dispose()
    }
    $result.elapsed_seconds = $runClock.Elapsed.TotalSeconds
    if ($workload) {
        $result.episodes = $workload.Episodes
        $result.validated_requests = $workload.Requests
        if ($result.elapsed_seconds -gt 0) { $result.requests_per_second = $workload.Requests / $result.elapsed_seconds }
        $result.latency_p50_ms = Get-LatencyPercentile $workload.Latency $workload.Requests 0.50
        $result.latency_p95_ms = Get-LatencyPercentile $workload.Latency $workload.Requests 0.95
        $result.latency_p99_ms = Get-LatencyPercentile $workload.Latency $workload.Requests 0.99
        if ($config.diagnostics) {
            $result.client_attempts = $workload.Connections.Issued
            $result.client_closures = $workload.Connections.Recorded
            $clientTrace = $workload.Connections.Snapshot()
            $result.client_records_overwritten = $result.client_closures - $clientTrace.Count
            $clientTrace | ForEach-Object { $_ | ConvertTo-Json -Compress } |
                Set-Content -LiteralPath (Join-Path $directory 'client-trace.jsonl')
        }
    }
    if ($resourceHistory.Count -gt 0) {
        $result.peak_private_bytes = ($resourceHistory | ForEach-Object { $_.private_bytes } | Measure-Object -Maximum).Maximum
        $result.peak_handles = ($resourceHistory | ForEach-Object { $_.handles } | Measure-Object -Maximum).Maximum
        $result.last_resource_sample = $resourceHistory[$resourceHistory.Count - 1]
    }
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath
    Write-Output $resultPath
}
