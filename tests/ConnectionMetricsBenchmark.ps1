[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $BaselineExe,
    [Parameter(Mandatory)][string] $CandidateExe,
    [ValidateRange(15, 63)][int] $Pairs = 15,
    [ValidateRange(1, 32)][int] $Clients = 8,
    [ValidateRange(1, 128)][int] $ConnectionsPerClient = 48,
    [ValidateRange(1, 1024)][int] $BurstsPerConnection = 12
)

$ErrorActionPreference = 'Stop'
$paths = @{
    baseline = (Resolve-Path -LiteralPath $BaselineExe).Path
    candidate = (Resolve-Path -LiteralPath $CandidateExe).Path
}

if (-not ('ConnectionBenchmarkClient' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'ConnectionBenchmarkClient.cs')
}

function Measure-Server([string] $ServerPath, [string] $Directory) {
    $metricsFile = Join-Path $Directory 'metrics.jsonl'
    if (Test-Path -LiteralPath $metricsFile) { Remove-Item -LiteralPath $metricsFile }
    $start = [Diagnostics.ProcessStartInfo]::new($ServerPath)
    $start.WorkingDirectory = $Directory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardOutput = $true
    $start.Environment['IOCP_ONNX_MAX_PAYLOAD_BYTES'] = '40'
    $start.Environment['IOCP_ONNX_MAX_CONNECTIONS'] = '256'
    $start.Environment['IOCP_ONNX_IDLE_TIMEOUT_MS'] = '60000'
    $start.Environment['IOCP_ONNX_FRAME_TIMEOUT_MS'] = '15000'
    $start.Environment['IOCP_ONNX_TIMEOUT_CHECK_INTERVAL_MS'] = '100'
    $server = [Diagnostics.Process]::Start($start)
    $output = $server.StandardOutput.ReadToEndAsync()
    $errors = $server.StandardError.ReadToEndAsync()
    try {
        $wait = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            if ($server.HasExited) { throw $errors.GetAwaiter().GetResult() }
            $probe = [Net.Sockets.TcpClient]::new([Net.Sockets.AddressFamily]::InterNetwork)
            try {
                if ($probe.ConnectAsync([Net.IPAddress]::Loopback, 8080).Wait(500)) { break }
            } catch [System.Management.Automation.MethodInvocationException] {
                if ($wait.ElapsedMilliseconds -ge 5000) { throw }
            } finally {
                $probe.Dispose()
            }
            if ($wait.ElapsedMilliseconds -ge 5000) { throw 'Benchmark server did not start.' }
            Start-Sleep -Milliseconds 20
        }

        $null = [ConnectionBenchmarkClient]::Run($Clients, 2, $BurstsPerConnection)
        return [ConnectionBenchmarkClient]::Run($Clients, $ConnectionsPerClient, $BurstsPerConnection)
    } finally {
        if (-not $server.HasExited) { $server.Kill() }
        if (-not $server.WaitForExit(5000)) { throw 'Benchmark server did not exit.' }
        $null = $output.GetAwaiter().GetResult()
        $null = $errors.GetAwaiter().GetResult()
        $server.Dispose()
    }
}

. (Join-Path $PSScriptRoot 'BenchmarkStatistics.ps1')

$workingDir = [IO.Directory]::CreateTempSubdirectory('iocp-connection-benchmark-').FullName
$modelDir = Join-Path $workingDir 'models'
try {
    $null = New-Item -ItemType Directory -Path $modelDir
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../models/model.onnx') -Destination $modelDir
    $samples = @{ baseline = @(); candidate = @() }
    $paired = [ordered]@{
        elapsed_ms = @(); requests_per_second = @(); p50_ms = @(); p95_ms = @(); p99_ms = @()
    }
    $requestCount = $Clients * $ConnectionsPerClient * $BurstsPerConnection * [ConnectionBenchmarkClient]::FramesPerBurst
    for ($pair = 1; $pair -le $Pairs; $pair++) {
        $order = if ($pair % 2) { 'baseline', 'candidate' } else { 'candidate', 'baseline' }
        $trials = @{}
        foreach ($variant in $order) {
            $result = Measure-Server $paths[$variant] $workingDir
            $trial = [ordered]@{
                event = 'trial'; pair = $pair; variant = $variant; elapsed_ms = $result.ElapsedMs
                requests_per_second = $requestCount * 1000 / $result.ElapsedMs
                p50_ms = $result.P50Ms; p95_ms = $result.P95Ms; p99_ms = $result.P99Ms
            }
            $trial | ConvertTo-Json -Compress
            $trials[$variant] = $trial
            $samples[$variant] += $trial
        }
        foreach ($metric in @($paired.Keys)) {
            $paired[$metric] += 100 * ($trials.candidate[$metric] / $trials.baseline[$metric] - 1)
        }
    }
    $estimates = [ordered]@{}
    $medians = [ordered]@{ baseline = [ordered]@{}; candidate = [ordered]@{} }
    foreach ($metric in $paired.Keys) {
        $estimates[$metric] = Get-PairedEstimate $paired[$metric]
        foreach ($variant in $paths.Keys) {
            $medians[$variant][$metric] = Get-Median @($samples[$variant] | ForEach-Object { $_[$metric] })
        }
    }
    [ordered]@{
        event = 'summary'
        pairs = $Pairs
        clients = $Clients
        connections_per_trial = $Clients * $ConnectionsPerClient
        requests_per_trial = $requestCount
        model_sha256 = (Get-FileHash -LiteralPath (Join-Path $modelDir 'model.onnx')).Hash
        baseline_sha256 = (Get-FileHash -LiteralPath $paths.baseline).Hash
        candidate_sha256 = (Get-FileHash -LiteralPath $paths.candidate).Hash
        medians = $medians
        paired_change = $estimates
    } | ConvertTo-Json -Compress -Depth 5
} finally {
    foreach ($path in @((Join-Path $modelDir 'model.onnx'), (Join-Path $workingDir 'metrics.jsonl'), $modelDir, $workingDir)) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
}
