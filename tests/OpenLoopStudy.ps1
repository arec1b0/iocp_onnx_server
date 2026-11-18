#Requires -Version 7.0
param(
    [Parameter(Mandatory)] [string] $Exe,
    [Parameter(Mandatory)] [string] $OutputDirectory,
    [string] $Reference = 'build/stage24-evidence/worker-results.json',
    [string] $Bundled = 'models/model.onnx',
    [string] $Dense256 = 'build/stage22-dense.onnx',
    [string] $Dense512 = 'build/stage23-evidence/dense512.onnx',
    [uint64[]] $ServerMasks = @(6, 30, 510),
    [uint64] $ClientMask = 15873,
    [int[]] $Workers = @(1, 2, 4, 8),
    [ValidateSet('bundled', 'dense256', 'dense512')] [string[]] $Models = @('bundled', 'dense256', 'dense512'),
    [ValidateSet('Fixed', 'Poisson')] [string[]] $Arrivals = @('Poisson'),
    [double] $Seconds = 3,
    [double] $WarmSeconds = 2,
    [double] $RecoverySeconds = 2,
    [int] $Connections = 72,
    [int] $Depth = 8,
    [int] $TimeoutMs = 750,
    [ValidateRange(2, 100)] [int] $DispatchDeadlineMs = 5,
    [int] $Seed = 2505,
    [double] $PilotRate = 0,
    [switch] $PilotRecovery
)
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $Exe).Path
$clientSources = 'ReliabilityHarness.cs', 'ConnectionBenchmarkClient.cs', 'OpenLoopBenchmarkClient.cs'
Add-Type -Path ($clientSources | ForEach-Object { Join-Path $PSScriptRoot $_ })
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh output directory.' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$manifest = [ordered]@{ executable = $exePath; exe_hash = (Get-FileHash $exePath).Hash; masks = $ServerMasks
    client_mask = $ClientMask; seed = $Seed; connections = $Connections; pipeline_depth = $Depth
    stopwatch_frequency = [Diagnostics.Stopwatch]::Frequency
    response_timeout_ms = $TimeoutMs; seconds = $Seconds; warm_seconds = $WarmSeconds; warm_fraction = 0.5
    dispatch_deadline_ms = $DispatchDeadlineMs
    recovery_seconds = $RecoverySeconds; queue_capacity = 64; connection_capacity = 256; profile = $true
    arrival_modes = $Arrivals; calibration_multipliers = @(0.65, 0.95, 1.15, 0.4)
    load_multipliers = @(0.5, 0.7, 0.85, 0.95, 1.1); pilot_rate = $PilotRate; models = @{} }
$configuration = @{ bundled = $Bundled; dense256 = $Dense256; dense512 = $Dense512 }
foreach ($model in $Models) {
    $path = (Resolve-Path -LiteralPath $configuration[$model]).Path
    $manifest.models[$model] = @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path).Hash }
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'manifest.json')
$referenceRows = if (-not $PilotRate) { Get-Content -LiteralPath $Reference -Raw | ConvertFrom-Json }
$controller = [Diagnostics.Process]::GetCurrentProcess()
$originalAffinity = [uint64]$controller.ProcessorAffinity.ToInt64()
foreach ($mask in $ServerMasks) {
    if (-not $mask -or -not $ClientMask -or ($mask -band $ClientMask) -or
        (($mask -bor $ClientMask) -band $originalAffinity) -ne ($mask -bor $ClientMask)) {
        throw 'Server and client masks must be nonempty, disjoint subsets of the controller CPU mask.'
    }
}
$env:IOCP_ONNX_MAX_CONNECTIONS = '256'
$env:IOCP_ONNX_MAX_PAYLOAD_BYTES = '40'
$env:IOCP_ONNX_IDLE_TIMEOUT_MS = '60000'
$env:IOCP_ONNX_FRAME_TIMEOUT_MS = '15000'
$env:IOCP_ONNX_TIMEOUT_CHECK_INTERVAL_MS = '100'
$env:IOCP_ONNX_INFERENCE_QUEUE_CAPACITY = '64'
$env:IOCP_ONNX_CONNECTION_TRACE = '0'
$env:IOCP_ONNX_INFERENCE_PROFILE = '1'

function Read-ConnectionMetrics([string] $Path) {
    Get-Content -LiteralPath $Path | Where-Object { $_.StartsWith('{"event":"connection_metrics"') } |
        ForEach-Object { ConvertFrom-Json $_ }
}

function Get-PhaseSummary($Phase) {
    if ($Phase.Offered -ne ($Phase.Attempted + $Phase.ScheduleMiss + $Phase.ClientFull) -or
        $Phase.Attempted -ne ($Phase.Successful + $Phase.Timeout + $Phase.Eof + $Phase.SocketError +
            $Phase.SendError + $Phase.BadResponse + $Phase.StreamDiscarded)) {
        throw "Offer accounting failed in $($Phase.Name)."
    }
    $cpuMs = $Phase.End.ServerCpuMs - $Phase.Begin.ServerCpuMs
    $clientMs = $Phase.End.ClientCpuMs - $Phase.Begin.ClientCpuMs
    $sampleMs = $Phase.End.ElapsedMs - $Phase.Begin.ElapsedMs
    $socketCodes = @{}
    foreach ($code in $Phase.SocketErrors.Keys) { $socketCodes[[string]$code] = $Phase.SocketErrors[$code] }
    [ordered]@{
        phase = $Phase.Name; target_rps = $Phase.TargetRate; seconds = $Phase.Seconds
        offered = $Phase.Offered; attempted = $Phase.Attempted; succeeded = $Phase.Successful; late = $Phase.Late
        schedule_miss = $Phase.ScheduleMiss; client_full = $Phase.ClientFull; timed_out = $Phase.Timeout
        eof = $Phase.Eof; socket_error = $Phase.SocketError; send_error = $Phase.SendError; stream_discarded = $Phase.StreamDiscarded
        bad_response = $Phase.BadResponse; socket_codes = $socketCodes
        offered_rps = $Phase.Offered / $Phase.Seconds; success_rps = $Phase.Successful / $Phase.Seconds
        success_inside_window = $Phase.WithinWindowSuccess
        error_pct = if ($Phase.Offered) { 100 * ($Phase.Offered - $Phase.Successful) / $Phase.Offered }
        timeout_pct = if ($Phase.Offered) { 100 * $Phase.Timeout / $Phase.Offered }
        p50_ms = $Phase.P50Ms; p95_ms = $Phase.P95Ms; p99_ms = $Phase.P99Ms
        wire_p50_ms = $Phase.WireP50Ms; wire_p95_ms = $Phase.WireP95Ms; wire_p99_ms = $Phase.WireP99Ms
        dispatch_lag_p50_ms = $Phase.LagP50Ms; dispatch_lag_p95_ms = $Phase.LagP95Ms; dispatch_lag_p99_ms = $Phase.LagP99Ms
        first_half_success_rps = $Phase.FirstHalfSuccessRate; last_half_success_rps = $Phase.LastHalfSuccessRate
        cpu_ms = $cpuMs; client_cpu_ms = $clientMs; sample_ms = $sampleMs
        cpu_seconds_per_1000 = if ($Phase.WithinWindowSuccess) { $cpuMs / $Phase.WithinWindowSuccess }
        server_cpu_cores = if ($sampleMs -gt 0) { $cpuMs / $sampleMs }
        client_cpu_cores = if ($sampleMs -gt 0) { $clientMs / $sampleMs }
        peak_client_outstanding_sampled = $Phase.PeakOutstanding; resources = @($Phase.Begin) + @($Phase.Samples) + @($Phase.End)
    }
}

function Invoke-OpenLoopTrial([string] $Label, [string] $Model, [uint64] $Mask, [int] $Count,
                              [string] $Arrival, [double] $Rate, [double] $BaseRate, [double] $Recovery,
                              [int] $TrialSeed, [double] $OperatingPoint) {
    $cpus = [System.Numerics.BitOperations]::PopCount($Mask)
    $directory = (New-Item -ItemType Directory -Path (Join-Path $output "${Model}-c${cpus}-w${Count}-$Label")).FullName
    $modelsDir = (New-Item -ItemType Directory -Path (Join-Path $directory 'models')).FullName
    Copy-Item -LiteralPath $manifest.models[$Model].path -Destination (Join-Path $modelsDir 'model.onnx')
    $stdout = Join-Path $directory 'server.stdout.log'
    $stderr = Join-Path $directory 'server.stderr.log'
    $env:IOCP_ONNX_INFERENCE_WORKERS = [string]$Count
    $server = [ReliabilityHarness]::StartServer($exePath, $directory, $stdout, $stderr, $Mask)
    try {
        $startup = [Diagnostics.Stopwatch]::StartNew()
        do {
            if ($server.HasExited) { throw "Server exited at startup ($([ReliabilityHarness]::ReadExitCode($server))). See $directory." }
            if ($startup.ElapsedMilliseconds -gt 5000) { throw "Server startup timed out. See $directory." }
            if (Select-String -LiteralPath $stdout -SimpleMatch '{"event":"connection_metrics","phase":"running"' -Quiet) { break }
            Start-Sleep -Milliseconds 10
        } while ($true)
        $threads = $server.Threads.Count
        if ([uint64]$server.ProcessorAffinity.ToInt64() -ne $Mask) { throw 'Server affinity did not match.' }
        $run = [OpenLoopBenchmarkClient]::Run($server, $Rate, $Seconds, [math]::Max(1, $BaseRate / 2),
            $WarmSeconds, [math]::Max(1, $BaseRate / 2), $Recovery, ($Arrival -eq 'Poisson'), $TrialSeed, $Connections,
            $Depth, $TimeoutMs, $DispatchDeadlineMs)
        $stop = [Diagnostics.Stopwatch]::StartNew()
        $exitCode = [ReliabilityHarness]::InterruptServer($server, 10000)
        $shutdownMs = $stop.Elapsed.TotalMilliseconds
        if ($exitCode -ne 0) { throw "Graceful shutdown returned $exitCode. See $directory." }
        $connectionMetrics = @(Read-ConnectionMetrics $stdout)
        foreach ($snapshot in $connectionMetrics) {
            if ($snapshot.connections_accepted_total - $snapshot.connections_closed_total -ne $snapshot.connections_active -or
                $snapshot.connections_high_watermark -gt 256) { throw 'Connection counts did not reconcile.' }
        }
        $final = $connectionMetrics | Where-Object phase -eq stopped | Select-Object -Last 1
        if (-not $final -or $final.connections_active -or $final.pending_io_operations) { throw 'Shutdown did not drain.' }
        $profile = Get-Content -LiteralPath (Join-Path $directory 'inference-profile.json') -Raw | ConvertFrom-Json
        $phases = @($run.Phases | ForEach-Object { Get-PhaseSummary $_ })
        $successes = ($run.Phases.Successful | Measure-Object -Sum).Sum
        $attempts = ($run.Phases.Attempted | Measure-Object -Sum).Sum
        if ($run.UnexpectedFrames -or ($phases | Where-Object bad_response -gt 0) -or
            $profile.admitted -ne ($profile.finished + $profile.cancelled) -or $successes -gt $profile.finished -or
            $profile.admitted + $profile.rejected -gt $attempts -or $profile.queue_high_watermark -gt 64) {
            throw "Response or scheduler accounting failed. See $directory."
        }
        $recordsFile = Join-Path $directory 'metrics.jsonl'
        [ConnectionBenchmarkClient]::ValidateMetricsFile($recordsFile, $profile.finished, $false)
        $run.SaveTrace((Join-Path $directory 'offers.tsv.gz'))
        $measurement = $phases | Where-Object phase -eq measurement
        # Establish a tested sustainable floor, with spare queue capacity and low dispatch distortion.
        $sustainable = $measurement.offered -gt 0 -and $measurement.error_pct -le 1 -and
            (100 * ($measurement.schedule_miss + $measurement.client_full) / $measurement.offered) -le 0.5 -and
            $null -ne $measurement.dispatch_lag_p99_ms -and $measurement.dispatch_lag_p99_ms -le 2 -and
            $profile.rejected -eq 0 -and $profile.queue_high_watermark -lt 64 -and
            $measurement.last_half_success_rps -ge 0.9 * $measurement.first_half_success_rps
        $busy = $profile.timings.worker_busy.total_ns
        $lifetime = $profile.timings.worker_lifetime.total_ns
        $row = [ordered]@{
            model = $Model; cpus = $cpus; affinity = $Mask; threads = $threads; workers = $Count
            label = $Label; directory = $directory; arrival = $Arrival; seed = $TrialSeed; load_fraction = $OperatingPoint
            calibration_floor_rps = $BaseRate; sustainable = $sustainable
            queue_mean_at_admission = if ($profile.admitted) { $profile.queue_depth_sum_at_admission / $profile.admitted }
            queue_high_watermark = $profile.queue_high_watermark
            worker_utilization_pct = if ($lifetime) { 100 * $busy / $lifetime }
            worker_wait_returns = $profile.wait_returns; worker_empty_wait_returns = $profile.empty_wait_returns
            queue_wait_mean_us = if ($profile.timings.queue_wait.count) { $profile.timings.queue_wait.total_ns / (1000 * $profile.timings.queue_wait.count) }
            execute_mean_us = if ($profile.timings.execute.count) { $profile.timings.execute.total_ns / (1000 * $profile.timings.execute.count) }
            metrics_lock_mean_us = if ($profile.timings.metrics_lock_wait.count) { $profile.timings.metrics_lock_wait.total_ns / (1000 * $profile.timings.metrics_lock_wait.count) }
            metrics_append_hold_mean_us = if ($profile.timings.metrics_append_hold.count) { $profile.timings.metrics_append_hold.total_ns / (1000 * $profile.timings.metrics_append_hold.count) }
            admitted = $profile.admitted; rejected = $profile.rejected; cancelled = $profile.cancelled; finished = $profile.finished
            jsonl_records_validated = $profile.finished; connection_final = $final
            client_drain_ms = $run.InPhaseDrainMs; shutdown_ms = $shutdownMs; client_reconnects = $run.Reconnects
            client_connect_failures = $run.ConnectFailures; measurement = $measurement; phases = $phases
        }
        $row | ConvertTo-Json -Depth 9 -Compress | Add-Content -LiteralPath (Join-Path $output 'trials.jsonl')
        # Retain each validated raw offer, profile and server log. Keep a representative metrics file per model.
        if ($Label -notin @('pilot-Fixed', 'pilot-Poisson', 'Poisson-50')) { Remove-Item -LiteralPath $recordsFile }
        Write-Host ("{0} c{1} w{2} {3}: {4:F0} offered, {5:F0} verified/s, {6:F2}% unsuccessful, p99={7}" -f
            $Model, $cpus, $Count, $Label, $measurement.offered_rps, $measurement.success_rps,
            $measurement.error_pct, $measurement.p99_ms)
        return $row
    } finally {
        if (-not $server.HasExited) { $server.Kill(); $null = $server.WaitForExit(5000) }
        $server.Dispose()
    }
}

try {
    $controller.ProcessorAffinity = [IntPtr]::new([long]$ClientMask)
    foreach ($model in $Models) {
        foreach ($mask in $ServerMasks) {
            $cpus = [System.Numerics.BitOperations]::PopCount($mask)
            foreach ($workerCount in $Workers) {
                if ($PilotRate) {
                    $recovery = if ($PilotRecovery) { $RecoverySeconds } else { 0 }
                    foreach ($arrival in $Arrivals) {
                        $null = Invoke-OpenLoopTrial "pilot-$arrival" $model $mask $workerCount $arrival $PilotRate $PilotRate $recovery $Seed 1
                    }
                    continue
                }
                $prior = @($referenceRows | Where-Object { $_.model -eq $model -and $_.cpus -eq $cpus -and $_.workers -eq $workerCount })
                if ($prior.Count -ne 1) { throw "Expected one calibration reference for $model c$cpus w$workerCount." }
                $calibrated = 0
                foreach ($fraction in $manifest.calibration_multipliers) {
                    if ($fraction -eq 0.4 -and $calibrated) { break }
                    if ($fraction -eq 1.15 -and $calibrated -lt 0.9 * $prior[0].rps) { continue }
                    $point = [int]($fraction * 100)
                    $rate = [math]::Round($prior[0].rps * $fraction)
                    $trial = Invoke-OpenLoopTrial "cal-$point" $model $mask $workerCount Fixed $rate $rate 0 $Seed 0
                    if ($trial.sustainable) { $calibrated = [math]::Max($calibrated, $rate) }
                }
                if (-not $calibrated) {
                    Write-Host "No sustainable floor for $model c$cpus w$workerCount. Points are relative to the lowest tested rate."
                }
                $floor = if ($calibrated) { $calibrated } else { [math]::Round(0.4 * $prior[0].rps) }
                @{ model = $model; cpus = $cpus; workers = $workerCount; sustainable_floor = $calibrated; rate_reference = $floor } |
                    ConvertTo-Json -Compress | Add-Content -LiteralPath (Join-Path $output 'calibration.jsonl')
                foreach ($arrival in $Arrivals) {
                    foreach ($fraction in $manifest.load_multipliers) {
                        $point = [int]($fraction * 100)
                        $recovery = if ($point -eq 110) { $RecoverySeconds } else { 0 }
                        $null = Invoke-OpenLoopTrial "$arrival-$point" $model $mask $workerCount $arrival `
                            ([math]::Round($floor * $fraction)) $floor $recovery $Seed $fraction
                    }
                }
            }
        }
    }
} finally {
    $controller.ProcessorAffinity = [IntPtr]::new([long]$originalAffinity)
    $controller.Dispose()
}
