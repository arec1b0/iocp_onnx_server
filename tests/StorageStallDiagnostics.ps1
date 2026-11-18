#Requires -Version 7.0
param(
    [Parameter(Mandatory)] [string] $ServerExe,
    [Parameter(Mandatory)] [string] $OutputDirectory,
    [ValidateRange(100, 2000)] [int] $HoldMs = 300,
    [ValidateRange(900, 5000)] [int] $ShutdownDeadlineMs = 1200
)
$ErrorActionPreference = 'Stop'
$exePath = (Resolve-Path -LiteralPath $ServerExe).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh output directory.' }
$root = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
Add-Type -Path @((Join-Path $PSScriptRoot 'ReliabilityHarness.cs'), (Join-Path $PSScriptRoot 'ConnectionBenchmarkClient.cs'))
$env:IOCP_ONNX_MAX_PAYLOAD_BYTES = '40'
$env:IOCP_ONNX_INFERENCE_WORKERS = '2'
$env:IOCP_ONNX_INFERENCE_QUEUE_CAPACITY = '2'
$env:IOCP_ONNX_MAX_CONNECTIONS = '64'
$env:IOCP_ONNX_IDLE_TIMEOUT_MS = '10000'
$env:IOCP_ONNX_INFERENCE_PROFILE = '1'
$env:IOCP_ONNX_CONNECTION_TRACE = '0'

function Read-EventRows([string] $Path, [string] $Name) {
    $prefix = '{"event":"' + $Name + '"'
    Get-Content -LiteralPath $Path | Where-Object { $_.Contains($prefix) } |
        ForEach-Object { $_.Substring($_.IndexOf($prefix)) | ConvertFrom-Json }
}

function Request-Exact([int] $Seed) {
    $client = [ReliabilityHarness]::BeginInference($Seed, 2500)
    try { [ReliabilityHarness]::VerifyInference($client, $Seed, 2500) }
    finally { $client.Dispose() }
}

function Resource-Snapshot($Server) {
    $Server.Refresh()
    @{ private_bytes = $Server.PrivateMemorySize64; handles = $Server.HandleCount; cpu_ms = $Server.TotalProcessorTime.TotalMilliseconds }
}

$results = @(foreach ($case in 'append-recovery', 'flush', 'close', 'partial', 'permanent', 'append-indefinite') {
    $directory = (New-Item -ItemType Directory -Path (Join-Path $root $case)).FullName
    $models = (New-Item -ItemType Directory -Path (Join-Path $directory 'models')).FullName
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../models/model.onnx') -Destination (Join-Path $models 'model.onnx')
    $prefix = 'Local\iocp-stage25-' + [guid]::NewGuid()
    $entered = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, "$prefix-entered")
    $release = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, "$prefix-release")
    $env:IOCP_STAGE25_EVENT = $prefix
    $env:IOCP_STAGE25_SINK = if ($case.StartsWith('append-')) { 'append' } else { $case }
    $stdout = Join-Path $directory 'stdout.log'
    $stderr = Join-Path $directory 'stderr.log'
    $metrics = Join-Path $directory 'metrics.jsonl'
    $server = $null
    $stopTask = $null
    $clients = [Collections.Generic.List[Net.Sockets.TcpClient]]::new()
    $row = [ordered]@{ case = $case; mechanism = 'diagnostic operation interception'; exact_responses = 0 }
    try {
        $server = [ReliabilityHarness]::StartServer($exePath, $directory, $stdout, $stderr)
        $startup = [Diagnostics.Stopwatch]::StartNew()
        do {
            if ($server.HasExited -or $startup.ElapsedMilliseconds -ge 5000) { throw "Server startup failed in $case." }
            if (Select-String -LiteralPath $stdout -SimpleMatch '{"event":"connection_metrics","phase":"running"' -Quiet) { break }
            Start-Sleep -Milliseconds 10
        } while ($true)
        $row.before = Resource-Snapshot $server
        if ($case.StartsWith('append-')) {
            $first = [ReliabilityHarness]::BeginInference(-5, 2500)
            $clients.Add($first)
            if (-not $entered.WaitOne(5000)) { throw 'Append gate was never entered.' }
            if ($case -eq 'append-indefinite') {
                # Observe the real shutdown request without ever releasing this gate.
                $watch = [Diagnostics.Stopwatch]::StartNew()
                try { $null = [ReliabilityHarness]::InterruptServer($server, $ShutdownDeadlineMs) }
                catch {
                    if ($_.Exception.InnerException -isnot [TimeoutException]) { throw }
                }
                $row.shutdown_observed_ms = $watch.Elapsed.TotalMilliseconds
                $row.alive_at_deadline = -not $server.HasExited
                $row.final_metrics_before_termination = @(Read-EventRows $stdout 'connection_metrics' |
                    Where-Object phase -eq stopped).Count
                if (-not $row.alive_at_deadline -or $row.final_metrics_before_termination) {
                    throw 'Expected shutdown to remain blocked while the append gate is closed.'
                }
                $row.held = Resource-Snapshot $server
                $server.Kill()
                if (-not $server.WaitForExit(5000)) { throw 'Isolated test process did not terminate.' }
                $row.terminated_by_harness = $true
                $row.metrics_length_after_termination = (Get-Item -LiteralPath $metrics).Length
            } else {
                # RST the first client after proving its callback reached the append gate.
                $first.Client.LingerState = [Net.Sockets.LingerOption]::new($true, 0)
                $first.Dispose()
                $ready = [Collections.Generic.Dictionary[int, int]]::new()
                $sentAt = @{}
                for ($i = 0; $i -lt 10; ++$i) {
                    $ready.Add($clients.Count, $i * 10 - 5)
                    $sentAt[$clients.Count] = [Diagnostics.Stopwatch]::StartNew()
                    $clients.Add([ReliabilityHarness]::BeginInference($i * 10 - 5, 2500))
                }
                $held = [Diagnostics.Stopwatch]::StartNew()
                $early = [Collections.Generic.Dictionary[int, int]]::new()
                do {
                    foreach ($i in $ready.Keys) {
                        if (-not $early.ContainsKey($i) -and $clients[$i].Client.Poll(0, [Net.Sockets.SelectMode]::SelectRead)) {
                            try {
                                if ($clients[$i].GetStream().ReadByte() -ne -1) { throw 'A response arrived while append was held.' }
                                $early.Add($i, 0)
                            } catch {
                                $errorCode = $_.Exception.GetBaseException()
                                if ($errorCode -isnot [Net.Sockets.SocketException] -or $errorCode.NativeErrorCode -notin 10053, 10054) { throw }
                                $early.Add($i, $errorCode.NativeErrorCode)
                            }
                        }
                    }
                    if ($held.ElapsedMilliseconds -ge 4000) { throw 'Expected at least one capacity rejection while writers are blocked.' }
                    if ($held.ElapsedMilliseconds -lt $HoldMs -or $early.Count -eq 0) { Start-Sleep -Milliseconds 10 }
                } while ($held.ElapsedMilliseconds -lt $HoldMs -or $early.Count -eq 0)
                $row.held_ms = $held.Elapsed.TotalMilliseconds
                $row.held = Resource-Snapshot $server
                $row.peer_close_while_held = @($early.Values)
                $null = $release.Set()
                $heldReplies = [Collections.Generic.List[double]]::new()
                foreach ($i in $ready.Keys) {
                    if ($early.ContainsKey($i)) { continue }
                    try {
                        [ReliabilityHarness]::VerifyInference($clients[$i], $ready[$i], 2500)
                        $heldReplies.Add($sentAt[$i].Elapsed.TotalMilliseconds)
                        $row.exact_responses++
                    } catch {
                        $problem = $_.Exception.GetBaseException()
                        if ($problem -isnot [IO.EndOfStreamException] -and
                            ($problem -isnot [Net.Sockets.SocketException] -or $problem.NativeErrorCode -notin 10053, 10054)) { throw }
                    }
                }
                $recoveredReplies = @(for ($i = 0; $i -lt 16; ++$i) {
                    $watch = [Diagnostics.Stopwatch]::StartNew()
                    Request-Exact -5
                    $row.exact_responses++
                    $watch.Elapsed.TotalMilliseconds
                })
                $row.held_reply_observed_ms = @($heldReplies)
                $row.recovered_round_trip_ms = $recoveredReplies
                $row.after = Resource-Snapshot $server
            }
        } elseif ($case -in 'flush', 'close') {
            Request-Exact -5
            $row.exact_responses++
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $stopTask = [ReliabilityHarness]::InterruptServerAsync($server, 10000)
            if (-not $entered.WaitOne(5000)) { throw "$case gate was never entered." }
            $row.held = Resource-Snapshot $server
            if ($server.WaitForExit($HoldMs)) { throw "Server exited while $case was held." }
            $row.alive_while_held = $true
            $null = $release.Set()
            $row.exit_code = $stopTask.GetAwaiter().GetResult()
            $row.shutdown_ms = $watch.Elapsed.TotalMilliseconds
        } else {
            # Two simultaneous producers encounter the injected failure and permanent fence.
            $clients.Add([ReliabilityHarness]::BeginInference(-5, 2500))
            $clients.Add([ReliabilityHarness]::BeginInference(5, 2500))
            [ReliabilityHarness]::VerifyInference($clients[0], -5, 2500)
            [ReliabilityHarness]::VerifyInference($clients[1], 5, 2500)
            $row.exact_responses = 2
            Request-Exact 15
            $row.exact_responses++
        }
        if ($case -ne 'append-indefinite') {
            if ($null -eq $stopTask) {
                foreach ($client in $clients) { $client.Dispose() }
                $row.exit_code = [ReliabilityHarness]::InterruptServer($server, 10000)
            }
            $end = Read-EventRows $stdout 'connection_metrics' | Where-Object phase -eq stopped | Select-Object -Last 1
            if (-not $end -or $end.connections_active -ne 0 -or $end.pending_io_operations -ne 0 -or
                $end.connections_accepted_total -ne $end.connections_closed_total) { throw "Lifecycle did not drain in $case." }
            $row.final = $end
            $profile = Get-Content -LiteralPath (Join-Path $directory 'inference-profile.json') -Raw | ConvertFrom-Json
            $row.profile = $profile
            if ($profile.admitted -ne ($profile.finished + $profile.cancelled) -or $profile.queue_high_watermark -gt 2) {
                throw "Queue accounting did not reconcile in $case."
            }
            if ($case -in 'partial', 'permanent') {
                $report = @(Read-EventRows $stderr 'inference_metrics_delivery_failed')
                $expected = if ($case -eq 'partial') { '{"times' } else { '' }
                if ($row.exit_code -ne 4 -or $report.Count -ne 1 -or $report[0].write_failures -ne 1 -or
                    $report[0].skipped -ne 2 -or $report[0].appended -ne 0 -or $report[0].flushed -ne 0 -or
                    [IO.File]::ReadAllText($metrics) -cne $expected) { throw "Unexpected failure report or file contents in $case." }
                $row.delivery = $report[0]
                $row.file_bytes = (Get-Item -LiteralPath $metrics).Length
            } else {
                if ($row.exit_code -ne 0) { throw "The $case case did not recover cleanly." }
                [ConnectionBenchmarkClient]::ValidateMetricsFile($metrics, $profile.finished, $false)
                $row.verified_records = $profile.finished
                if ($case -eq 'append-recovery' -and ($profile.rejected -lt $early.Count -or
                    $profile.queue_high_watermark -ne 2 -or $profile.timings.metrics_lock_wait.max_ns -lt ($HoldMs / 2 * 1e6))) {
                    throw 'Expected queue saturation and a second producer blocked on the metrics mutex.'
                }
            }
        }
        Write-Host "$case verified"
        $row
    } finally {
        foreach ($client in $clients) { $client.Dispose() }
        if ($case -ne 'append-indefinite') { $null = $release.Set() }
        if ($server -and -not $server.HasExited) { $server.Kill(); $null = $server.WaitForExit(5000) }
        if ($stopTask) { try { $null = $stopTask.GetAwaiter().GetResult() } catch { } }
        if ($server) { $server.Dispose() }
        $entered.Dispose()
        $release.Dispose()
    }
})
$results | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $root 'result.json')
