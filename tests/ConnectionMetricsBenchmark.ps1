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
    Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading.Tasks;

public static class ConnectionBenchmarkClient
{
    public const int FramesPerBurst = 8;
    private const int FrameBytes = 48;
    private const int TimeoutMs = 5000;

    private static byte[] BuildBurst(int multiplier)
    {
        var bytes = new byte[FramesPerBurst * FrameBytes];
        for (var frame = 0; frame < FramesPerBurst; ++frame)
        {
            var offset = frame * FrameBytes;
            BitConverter.GetBytes(10UL).CopyTo(bytes, offset);
            for (var element = 0; element < 10; ++element)
            {
                float value = multiplier * (frame * 10 + element - 5);
                BitConverter.GetBytes(value).CopyTo(bytes, offset + 8 + 4 * element);
            }
        }
        return bytes;
    }

    public sealed class Trial
    {
        public double ElapsedMs;
        public double P50Ms;
        public double P95Ms;
        public double P99Ms;
    }

    public static Trial Run(int clients, int connectionsPerClient, int burstsPerConnection)
    {
        var request = BuildBurst(1);
        var expected = BuildBurst(2);
        var requestsPerClient = connectionsPerClient * burstsPerConnection * FramesPerBurst;
        var latencies = new long[clients * requestsPerClient];
        var stopwatch = Stopwatch.StartNew();
        var options = new ParallelOptions { MaxDegreeOfParallelism = clients };
        Parallel.For(0, clients, options, worker =>
        {
            var response = new byte[expected.Length];
            var nextLatency = worker * requestsPerClient;
            for (var connection = 0; connection < connectionsPerClient; ++connection)
            {
                using (var client = new TcpClient(AddressFamily.InterNetwork))
                {
                    client.NoDelay = true;
                    client.SendTimeout = TimeoutMs;
                    client.ReceiveTimeout = TimeoutMs;
                    if (!client.ConnectAsync(IPAddress.Loopback, 8080).Wait(TimeoutMs))
                    {
                        throw new IOException("Benchmark connect timed out.");
                    }
                    var stream = client.GetStream();
                    for (var burst = 0; burst < burstsPerConnection; ++burst)
                    {
                        var issued = Stopwatch.GetTimestamp();
                        stream.Write(request, 0, request.Length);
                        var offset = 0;
                        while (offset < response.Length)
                        {
                            var read = stream.Read(response, offset, response.Length - offset);
                            if (read == 0)
                            {
                                throw new IOException("Benchmark response was truncated.");
                            }
                            var end = offset + read;
                            if (end / FrameBytes > offset / FrameBytes)
                            {
                                var latency = Stopwatch.GetTimestamp() - issued;
                                for (var frame = offset / FrameBytes; frame < end / FrameBytes; ++frame)
                                {
                                    latencies[nextLatency++] = latency;
                                }
                            }
                            offset = end;
                        }
                        if (!response.AsSpan().SequenceEqual(expected))
                        {
                            throw new IOException("Benchmark response was incorrect.");
                        }
                    }
                    client.Client.Shutdown(SocketShutdown.Send);
                    if (stream.ReadByte() != -1)
                    {
                        throw new IOException("Benchmark connection produced an extra response.");
                    }
                }
            }
        });
        stopwatch.Stop();
        Array.Sort(latencies);
        double Percentile(double rank) => latencies[(int)Math.Ceiling(rank * latencies.Length) - 1] * 1000.0 / Stopwatch.Frequency;
        return new Trial
        {
            ElapsedMs = stopwatch.Elapsed.TotalMilliseconds,
            P50Ms = Percentile(0.50),
            P95Ms = Percentile(0.95),
            P99Ms = Percentile(0.99)
        };
    }
}
'@
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

function Get-Median([double[]] $Values) {
    $sorted = [double[]]$Values.Clone()
    [Array]::Sort($sorted)
    $middle = [int][Math]::Floor($sorted.Length / 2)
    if ($sorted.Length % 2) { return $sorted[$middle] }
    return ($sorted[$middle - 1] + $sorted[$middle]) / 2
}

function Get-PairedEstimate([double[]] $Changes) {
    $median = Get-Median $Changes
    $random = [Random]::new(1701)
    $medians = [double[]]::new(10000)
    $resample = [double[]]::new($Changes.Length)
    for ($i = 0; $i -lt $medians.Length; ++$i) {
        for ($j = 0; $j -lt $resample.Length; ++$j) { $resample[$j] = $Changes[$random.Next($Changes.Length)] }
        $medians[$i] = Get-Median $resample
    }
    [Array]::Sort($medians)
    [ordered]@{
        median_pct = [Math]::Round($median, 3)
        bootstrap_95_pct = @([Math]::Round($medians[249], 3), [Math]::Round($medians[9749], 3))
    }
}

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
