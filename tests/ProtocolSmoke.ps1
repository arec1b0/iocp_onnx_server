[CmdletBinding()]
param([Parameter(Mandatory)][string] $ServerExe)

$ErrorActionPreference = 'Stop'
$serverPath = (Resolve-Path -LiteralPath $ServerExe).Path
$workingDir = [IO.Directory]::CreateTempSubdirectory('iocp-protocol-')
$modelDir = Join-Path $workingDir.FullName 'models'

function Start-TestServer([string] $Limit) {
    $start = [Diagnostics.ProcessStartInfo]::new($serverPath)
    $start.WorkingDirectory = $workingDir.FullName
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardError = $true
    $start.Environment['IOCP_ONNX_MAX_PAYLOAD_BYTES'] = $Limit
    [Diagnostics.Process]::Start($start)
}

function Stop-TestServer([Diagnostics.Process] $Process) {
    if (-not $Process.HasExited) {
        $Process.Kill()
        if (-not $Process.WaitForExit(5000)) { throw 'Test server did not exit.' }
    }
    $Process.Dispose()
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

try {
    $null = New-Item -ItemType Directory -Path $modelDir
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../models/model.onnx') -Destination $modelDir

    foreach ($limit in @('39', '40x', '4294967288', '4294967296')) {
        $server = Start-TestServer $limit
        $errors = $server.StandardError.ReadToEndAsync()
        try {
            if (-not $server.WaitForExit(5000) -or $server.ExitCode -ne 1) {
                throw "Expected startup to fail for payload limit $limit."
            }
            $expected = if ($limit -eq '39') { 'Model input or output exceeds' } else { 'must be a decimal integer' }
            if ($errors.GetAwaiter().GetResult() -notlike "*$expected*") {
                throw "Startup rejected payload limit $limit for an unexpected reason."
            }
        } finally {
            Stop-TestServer $server
        }
    }

    $server = Start-TestServer '40'
    $errors = $server.StandardError.ReadToEndAsync()
    try {
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
        Test-ValidRequests
        if ($server.HasExited) { throw 'Server exited during the protocol test.' }
    } finally {
        Stop-TestServer $server
    }
    Write-Host 'TCP protocol smoke test passed.'
} finally {
    foreach ($path in @((Join-Path $modelDir 'model.onnx'), (Join-Path $workingDir.FullName 'metrics.jsonl'), $modelDir, $workingDir.FullName)) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path
        }
    }
}
