[CmdletBinding()]
param([Parameter(Mandatory)][string] $ServerExe)

$ErrorActionPreference = 'Stop'
$serverPath = (Resolve-Path -LiteralPath $ServerExe).Path
$workingDir = [IO.Directory]::CreateTempSubdirectory('iocp-protocol-')
$modelDir = Join-Path $workingDir.FullName 'models'

function Start-TestServer([string] $Limit, [hashtable] $Overrides = @{}) {
    $start = [Diagnostics.ProcessStartInfo]::new($serverPath)
    $start.WorkingDirectory = $workingDir.FullName
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardOutput = $true
    $start.Environment['IOCP_ONNX_MAX_PAYLOAD_BYTES'] = $Limit
    $start.Environment['IOCP_ONNX_MAX_CONNECTIONS'] = '4'
    $start.Environment['IOCP_ONNX_IDLE_TIMEOUT_MS'] = '5000'
    $start.Environment['IOCP_ONNX_FRAME_TIMEOUT_MS'] = '3000'
    $start.Environment['IOCP_ONNX_TIMEOUT_CHECK_INTERVAL_MS'] = '25'
    foreach ($setting in $Overrides.GetEnumerator()) {
        $start.Environment[$setting.Key] = $setting.Value
    }
    [Diagnostics.Process]::Start($start)
}

function Stop-TestServer([Diagnostics.Process] $Process) {
    if (-not $Process.HasExited) {
        $Process.Kill()
        if (-not $Process.WaitForExit(5000)) { throw 'Test server did not exit.' }
    }
    $Process.Dispose()
}

function Assert-StartupRejected([string] $Limit, [hashtable] $Overrides, [string] $Expected) {
    $server = Start-TestServer $Limit $Overrides
    $errors = $server.StandardError.ReadToEndAsync()
    try {
        if (-not $server.WaitForExit(5000) -or $server.ExitCode -ne 1) {
            throw "Expected startup to reject $Expected."
        }
        if ($errors.GetAwaiter().GetResult() -notlike "*$Expected*") {
            throw "Startup rejected the configuration for an unexpected reason. Expected $Expected."
        }
    } finally {
        Stop-TestServer $server
    }
}

function Assert-InitialConnectionMetrics([Diagnostics.Process] $Server) {
    $budget = [TimeSpan]::FromSeconds(5)
    $wait = [Diagnostics.Stopwatch]::StartNew()
    while (($remaining = $budget - $wait.Elapsed) -gt [TimeSpan]::Zero) {
        $pendingLine = $Server.StandardOutput.ReadLineAsync()
        if (-not $pendingLine.Wait($remaining)) { break }
        $line = $pendingLine.GetAwaiter().GetResult()
        if ($null -eq $line) { break }
        if (-not $line.StartsWith('{"event":"connection_metrics"')) { continue }
        $metrics = $line | ConvertFrom-Json
        if ($metrics.phase -ne 'running' -or $metrics.timestamp_ms -le 0) {
            throw 'Initial connection metrics had the wrong phase or timestamp.'
        }
        foreach ($name in @(
            'connections_accepted_total', 'connections_rejected_capacity_total',
            'connections_closed_total', 'connections_active', 'connections_high_watermark',
            'idle_timeouts_total', 'incomplete_frame_timeouts_total', 'send_timeouts_total',
            'io_failures_total', 'pending_io_operations', 'cancellation_requests_total',
            'cancellation_completions_total'
        )) {
            if ($metrics.$name -ne 0) { throw "Expected an initial zero value for $name." }
        }
        Write-Host 'Structured connection metrics startup record passed.'
        return
    }
    throw 'Server did not emit its initial connection metrics snapshot.'
}

function Connect-TestClient {
    $client = [Net.Sockets.TcpClient]::new([Net.Sockets.AddressFamily]::InterNetwork)
    try {
        $client.Connect([Net.IPAddress]::Loopback, 8080)
        $client.NoDelay = $true
        $client.ReceiveTimeout = 2500
        $client.SendTimeout = 2500
        return $client
    } catch {
        $client.Dispose()
        throw
    }
}

function Read-Exactly([IO.Stream] $Stream, [int] $Length) {
    $buffer = [byte[]]::new($Length)
    $offset = 0
    while ($offset -lt $Length) {
        $read = $Stream.Read($buffer, $offset, [Math]::Min(3, $Length - $offset))
        if ($read -eq 0) { throw 'Connection closed during a response.' }
        $offset += $read
    }
    return ,$buffer
}

function New-RequestFrame([int] $Seed) {
    $frame = [byte[]]::new(48)
    [BitConverter]::GetBytes([uint64]10).CopyTo($frame, 0)
    foreach ($i in 0..9) {
        [BitConverter]::GetBytes([single]($Seed + $i - 4.5)).CopyTo($frame, 8 + 4 * $i)
    }
    return ,$frame
}

function Assert-Response([IO.Stream] $Stream, [int] $Seed) {
    if ([BitConverter]::ToUInt64((Read-Exactly $Stream 8), 0) -ne 10) {
        throw 'Unexpected response element count.'
    }
    $payload = Read-Exactly $Stream 40
    foreach ($i in 0..9) {
        if ([BitConverter]::ToSingle($payload, 4 * $i) -ne 2 * ($Seed + $i - 4.5)) {
            throw "Unexpected response for request $Seed at index $i."
        }
    }
}

function Test-ValidRequests {
    $client = Connect-TestClient
    try {
        $stream = $client.GetStream()
        foreach ($split in @(3, 21, 35)) {
            $frame = New-RequestFrame $split
            $stream.Write($frame, 0, $split)
            Start-Sleep -Milliseconds 20
            $stream.Write($frame, $split, $frame.Length - $split)
            Assert-Response $stream $split
        }
    } finally {
        $client.Dispose()
    }
}

function Test-CoalescedRequests {
    $client = Connect-TestClient
    try {
        $stream = $client.GetStream()
        $seeds = @(101, -45, 700, 16)
        $batch = [byte[]]::new(48 * $seeds.Count)
        for ($i = 0; $i -lt $seeds.Count; $i++) {
            (New-RequestFrame $seeds[$i]).CopyTo($batch, 48 * $i)
        }
        $stream.Write($batch, 0, $batch.Length)
        foreach ($seed in $seeds) {
            Assert-Response $stream $seed
        }

        $client.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
        Assert-ConnectionClosed $stream 'Coalesced requests after the final expected response'
        Write-Host 'Four coalesced requests returned four ordered responses.'
    } finally {
        $client.Dispose()
    }
}

function Assert-ConnectionClosed([IO.Stream] $Stream, [string] $Label) {
    try {
        $responseByte = $Stream.ReadByte()
    } catch {
        $cause = $_.Exception.GetBaseException()
        if ($cause -is [Net.Sockets.SocketException] -and $cause.SocketErrorCode -eq 'ConnectionReset') {
            return
        }
        throw
    }
    if ($responseByte -ne -1) { throw "$Label produced a response." }
}

function Test-RejectedPrefix([uint64] $Count) {
    $client = Connect-TestClient
    try {
        $stream = $client.GetStream()
        $prefix = [BitConverter]::GetBytes($Count)
        $stream.Write($prefix, 0, 7)
        Start-Sleep -Milliseconds 20
        $stream.Write($prefix, 7, 1)
        Assert-ConnectionClosed $stream "Rejected count $Count"
    } finally {
        $client.Dispose()
    }
}

function Test-TruncatedRequest([int] $Length) {
    $client = Connect-TestClient
    try {
        $stream = $client.GetStream()
        $frame = [byte[]]::new(48)
        [BitConverter]::GetBytes([uint64]10).CopyTo($frame, 0)
        $stream.Write($frame, 0, $Length)
        $client.Client.Shutdown([Net.Sockets.SocketShutdown]::Send)
        Assert-ConnectionClosed $stream "Request truncated to $Length bytes"
    } finally {
        $client.Dispose()
    }
}

function Test-UnexpectedClientExit {
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $PSHOME 'pwsh.exe'))
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $script = @'
$ErrorActionPreference = 'Stop'
$client = [Net.Sockets.TcpClient]::new([Net.Sockets.AddressFamily]::InterNetwork)
if (-not $client.ConnectAsync([Net.IPAddress]::Loopback, 8080).Wait(2500)) { exit 1 }
$client.ReceiveTimeout = 2500
$client.SendTimeout = 2500
$stream = $client.GetStream()
$frame = [byte[]]::new(48)
$frame[0] = 10
$stream.Write($frame)
$stream.ReadExactly([byte[]]::new(48))
$stream.Write($frame, 0, 3)
[Console]::WriteLine('ready')
[Console]::Out.Flush()
$null = [Threading.ManualResetEventSlim]::new().Wait(10000)
exit 1
'@
    foreach ($argument in @('-NoProfile', '-NonInteractive', '-EncodedCommand',
            [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script)))) {
        $start.ArgumentList.Add($argument)
    }
    $child = [Diagnostics.Process]::Start($start)
    try {
        $ready = $child.StandardOutput.ReadLineAsync()
        if (-not $ready.Wait(5000) -or $ready.Result -ne 'ready' -or $child.HasExited) {
            throw 'Client process did not establish a persistent connection before termination.'
        }
        $child.Kill()
        if (-not $child.WaitForExit(5000)) { throw 'Terminated client did not exit.' }
    } finally {
        Stop-TestServer $child
    }
    Test-ValidRequests
    Write-Host 'Server continued to serve exact responses after the client process was terminated.'
}

try {
    $null = New-Item -ItemType Directory -Path $modelDir
    Assert-StartupRejected '40' @{} 'Failed to load model.'
    Write-Host 'Missing model caused a clean startup failure.'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../models/model.onnx') -Destination $modelDir

    foreach ($limit in @('39', '40x', '4294967288', '4294967296')) {
        $expected = if ($limit -eq '39') { 'Model input or output exceeds' } else {
            'IOCP_ONNX_MAX_PAYLOAD_BYTES must be a decimal integer'
        }
        Assert-StartupRejected $limit @{} $expected
    }
    $invalidSettings = @{
        IOCP_ONNX_MAX_CONNECTIONS = @('0', '65536')
        IOCP_ONNX_IDLE_TIMEOUT_MS = @('-1', '86400001')
        IOCP_ONNX_FRAME_TIMEOUT_MS = @('0', '1.5', '86400001')
        IOCP_ONNX_TIMEOUT_CHECK_INTERVAL_MS = @('0', '1001')
        IOCP_ONNX_INFERENCE_WORKERS = @('0', '65')
        IOCP_ONNX_INFERENCE_QUEUE_CAPACITY = @('0', '4097')
    }
    foreach ($setting in $invalidSettings.GetEnumerator()) {
        foreach ($value in $setting.Value) {
            $override = @{}
            $override[$setting.Key] = $value
            Assert-StartupRejected '40' $override "$($setting.Key) must be a decimal integer"
        }
    }

    $server = Start-TestServer '40'
    $errors = $server.StandardError.ReadToEndAsync()
    try {
        Assert-InitialConnectionMetrics $server
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            if ($server.HasExited) { throw "Server exited with code $($server.ExitCode). $($errors.GetAwaiter().GetResult())" }
            try {
                (Connect-TestClient).Dispose()
                break
            } catch {
                if ($deadline.ElapsedMilliseconds -ge 5000) { throw }
                Start-Sleep -Milliseconds 50
            }
        }

        Test-ValidRequests
        Test-CoalescedRequests
        foreach ($invalidCount in @([uint64]0, [uint64]9, [uint64]11, ([uint64]1 -shl 62), [uint64]::MaxValue)) {
            Test-RejectedPrefix $invalidCount
        }
        Test-TruncatedRequest 7
        Test-TruncatedRequest 11
        Test-UnexpectedClientExit
        if ($server.HasExited) { throw 'Server exited during the protocol test.' }
    } finally {
        Stop-TestServer $server
    }
    Write-Host 'TCP protocol smoke test passed with configured connection limits and timeouts.'
} finally {
    foreach ($path in @((Join-Path $modelDir 'model.onnx'), (Join-Path $workingDir.FullName 'metrics.jsonl'), $modelDir, $workingDir.FullName)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path
        }
    }
}
