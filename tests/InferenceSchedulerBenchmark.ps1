[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $CandidateExe,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $Model = (Join-Path $PSScriptRoot '../models/model.onnx'),
    [ValidateSet('Compare', 'Sweep', 'ProfileOverhead')][string] $Mode = 'Compare',
    [string] $Stage1Exe,
    [string] $ReferenceExe,
    [string] $ReferenceLabel = 'stage21',
    [string] $CandidateLabel = 'candidate',
    [switch] $ProfileBoth,
    [switch] $ContextSwitches,
    [ValidateRange(1, 63)][int] $Rounds = 15,
    [ValidateRange(1, 32)][int] $Clients = 8,
    [ValidateRange(1, 128)][int] $ConnectionsPerClient = 4,
    [ValidateRange(1, 1024)][int] $BurstsPerConnection = 12,
    [ValidateRange(1, 64)][int[]] $Workers = @(8),
    [ValidateRange(1, 4096)][int[]] $Queues = @(64)
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BenchmarkStatistics.ps1')
Add-Type -Path (Join-Path $PSScriptRoot 'ReliabilityHarness.cs')
Add-Type -Path (Join-Path $PSScriptRoot 'ConnectionBenchmarkClient.cs')

function New-Variant([string] $Label, [string] $Exe, [int] $WorkerCount, [int] $Queue, [bool] $Profile) {
    [pscustomobject]@{
        Label = $Label; Exe = (Resolve-Path -LiteralPath $Exe).Path; Workers = $WorkerCount
        Queue = $Queue; Profile = $Profile; Sha256 = (Get-FileHash -LiteralPath $Exe).Hash
    }
}

$variants = @(
    if ($Mode -eq 'Sweep') {
        foreach ($workerCount in $Workers) {
            foreach ($queue in $Queues) {
                New-Variant "w${workerCount}-q${queue}" $CandidateExe $workerCount $queue $true
            }
        }
    } elseif ($Mode -eq 'ProfileOverhead') {
        New-Variant 'profile_off' $CandidateExe $Workers[0] $Queues[0] $false
        New-Variant 'profile_on' $CandidateExe $Workers[0] $Queues[0] $true
    } else {
        if ($Stage1Exe) { New-Variant 'stage1' $Stage1Exe $Workers[0] $Queues[0] $false }
        if ($ReferenceExe) { New-Variant $ReferenceLabel $ReferenceExe $Workers[0] $Queues[0] $ProfileBoth }
        New-Variant $CandidateLabel $CandidateExe $Workers[0] $Queues[0] $ProfileBoth
    }
)
if (@($variants.Label | Sort-Object -Unique).Count -ne $variants.Count) { throw 'Variant labels must be unique.' }

$modelPath = (Resolve-Path -LiteralPath $Model).Path
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a fresh output directory.' }
$output = (New-Item -ItemType Directory -Path $OutputDirectory).FullName
$models = (New-Item -ItemType Directory -Path (Join-Path $output 'models')).FullName
Copy-Item -LiteralPath $modelPath -Destination (Join-Path $models 'model.onnx')
$stdout = Join-Path $output 'server.stdout.log'
$stderr = Join-Path $output 'server.stderr.log'
$trace = Join-Path $output 'inference-profile.json'
$metrics = Join-Path $output 'metrics.jsonl'
$jsonl = Join-Path $output 'trials.jsonl'

# The harness runs in its own PowerShell process and launches a dedicated server console.
$env:IOCP_ONNX_MAX_CONNECTIONS = '256'
$env:IOCP_ONNX_MAX_PAYLOAD_BYTES = '40'
$env:IOCP_ONNX_IDLE_TIMEOUT_MS = '60000'
$env:IOCP_ONNX_FRAME_TIMEOUT_MS = '15000'
$env:IOCP_ONNX_TIMEOUT_CHECK_INTERVAL_MS = '100'
$env:IOCP_ONNX_CONNECTION_TRACE = '0'

function Get-SwitchSnapshot([int] $ProcessId) {
    $snapshot = @{}
    foreach ($entry in Get-CimInstance Win32_PerfRawData_PerfProc_Thread -Filter "IDProcess = $ProcessId" -OperationTimeoutSec 5) {
        $snapshot[[uint32]$entry.IDThread] = [long]$entry.ContextSwitchesPersec
    }
    if ($snapshot.Count -eq 0) { throw 'No process thread counters were available.' }
    return $snapshot
}

function Invoke-Trial($Variant, [int] $Round) {
    foreach ($file in $stdout, $stderr, $trace, $metrics) {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
    }
    $env:IOCP_ONNX_INFERENCE_WORKERS = [string]$Variant.Workers
    $env:IOCP_ONNX_INFERENCE_QUEUE_CAPACITY = [string]$Variant.Queue
    $env:IOCP_ONNX_INFERENCE_PROFILE = if ($Variant.Profile) { '1' } else { '0' }
    $allowReject = $Mode -eq 'Sweep' -and $Variant.Queue -lt $Clients
    $server = [ReliabilityHarness]::StartServer($Variant.Exe, $output, $stdout, $stderr)
    try {
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            if ($server.HasExited) { throw "Server exited at startup with code $($server.ExitCode)." }
            $probe = [Net.Sockets.TcpClient]::new([Net.Sockets.AddressFamily]::InterNetwork)
            try {
                if ($probe.ConnectAsync([Net.IPAddress]::Loopback, 8080).Wait(500)) { break }
            } catch [System.Management.Automation.MethodInvocationException] {
                if ($deadline.ElapsedMilliseconds -ge 5000) { throw }
            } finally { $probe.Dispose() }
            if ($deadline.ElapsedMilliseconds -ge 5000) { throw 'Benchmark server did not start.' }
            Start-Sleep -Milliseconds 20
        }

        $warm = [ConnectionBenchmarkClient]::Run($Clients, 1, $BurstsPerConnection, $allowReject)
        if ($ContextSwitches) { $beforeSwitch = Get-SwitchSnapshot $server.Id }
        $server.Refresh()
        $beforeCpu = $server.TotalProcessorTime.TotalMilliseconds
        $sample = [ConnectionBenchmarkClient]::Run($Clients, $ConnectionsPerClient, $BurstsPerConnection, $allowReject)
        $server.Refresh()
        $cpu = $server.TotalProcessorTime.TotalMilliseconds - $beforeCpu
        $privateBytes = $server.PrivateMemorySize64
        $switches = $null
        if ($ContextSwitches) {
            $afterSwitch = Get-SwitchSnapshot $server.Id
            $switches = [long]0
            foreach ($id in $afterSwitch.Keys) {
                if ($beforeSwitch.ContainsKey($id)) {
                    # The raw per-thread counter is uint32 and can wrap between samples.
                    $switches += ($afterSwitch[$id] - $beforeSwitch[$id]) -band 0xFFFFFFFFL
                }
            }
        }

        $exitCode = [ReliabilityHarness]::InterruptServer($server, 10000)
        if ($exitCode -ne 0) { throw "Graceful shutdown failed with code $exitCode." }
        $final = Get-Content -LiteralPath $stdout | Where-Object { $_.StartsWith('{"event":"connection_metrics"') } |
            ForEach-Object { ConvertFrom-Json $_ } | Where-Object phase -eq 'stopped' | Select-Object -Last 1
        if (-not $final -or $final.connections_active -ne 0 -or $final.pending_io_operations -ne 0 -or
            $final.connections_accepted_total -ne $final.connections_closed_total) { throw 'Shutdown did not drain.' }

        $profile = if ($Variant.Profile) { Get-Content -LiteralPath $trace -Raw | ConvertFrom-Json }
        if ($profile) {
            $failures = $warm.EarlyEof + $warm.Reset + $warm.Aborted + $sample.EarlyEof + $sample.Reset + $sample.Aborted
            if ($profile.rejected -ne $failures -or $profile.admitted -ne ($profile.finished + $profile.cancelled) -or
                $profile.queue_high_watermark -gt $Variant.Queue -or
                $profile.timings.response.count -ne ($warm.Completed + $sample.Completed)) {
                throw 'Client output and the scheduler accounting did not reconcile.'
            }
        }
        [ordered]@{
            round = $Round; variant = $Variant.Label; workers = $Variant.Workers; capacity = $Variant.Queue
            completed = $sample.Completed; early_eof = $sample.EarlyEof; reset_10054 = $sample.Reset; aborted_10053 = $sample.Aborted
            elapsed_ms = $sample.ElapsedMs; requests_per_second = 1000 * $sample.Completed / $sample.ElapsedMs
            p50_ms = $sample.P50Ms; p95_ms = $sample.P95Ms; p99_ms = $sample.P99Ms
            cpu_ms = $cpu; cpu_cores = $cpu / $sample.ElapsedMs; private_bytes = $privateBytes
            context_switches = $switches; profile = $profile
        }
    } finally {
        if (-not $server.HasExited) { $server.Kill(); $null = $server.WaitForExit(5000) }
        $server.Dispose()
    }
}

$samples = [Collections.Generic.List[object]]::new()
$random = [Random]::new(2201)
for ($round = 1; $round -le $Rounds; ++$round) {
    $order = $variants.Clone()
    if ($Mode -eq 'Sweep') {
        for ($i = $order.Count - 1; $i -gt 0; --$i) {
            $j = $random.Next($i + 1)
            $order[$i], $order[$j] = $order[$j], $order[$i]
        }
    } elseif ($order.Count -gt 1) {
        $start = ($round - 1) % $order.Count
        $order = for ($i = 0; $i -lt $variants.Count; ++$i) { $variants[($start + $i) % $variants.Count] }
    }
    foreach ($variant in $order) {
        try { $trial = Invoke-Trial $variant $round }
        catch {
            $failure = "Trial $round ($($variant.Label)) failed. $($_.Exception.Message)"
            $failure | Set-Content -LiteralPath (Join-Path $output 'failure.txt')
            throw $failure
        }
        $samples.Add($trial)
        $trial | ConvertTo-Json -Depth 8 -Compress | Add-Content -LiteralPath $jsonl
    }
}

$measures = 'requests_per_second', 'p50_ms', 'p95_ms', 'p99_ms', 'cpu_cores', 'cpu_ms', 'private_bytes', 'context_switches'
$medians = [ordered]@{}
foreach ($variant in $variants) {
    $group = @($samples | Where-Object variant -eq $variant.Label)
    $medians[$variant.Label] = [ordered]@{
        completed = Get-Median @($group.completed)
        rejected_connections = Get-Median @($group | ForEach-Object { $_.early_eof + $_.reset_10054 + $_.aborted_10053 })
    }
    foreach ($measure in $measures) {
        if ($null -ne $group[0][$measure]) { $medians[$variant.Label][$measure] = Get-Median @($group.$measure) }
    }
}

$paired = [ordered]@{}
if ($Mode -ne 'Sweep') {
    for ($i = 0; $i -lt $variants.Count; ++$i) {
        for ($j = $i + 1; $j -lt $variants.Count; ++$j) {
            $baseName = $variants[$i].Label
            $newName = $variants[$j].Label
            $base = @($samples | Where-Object variant -eq $baseName | Sort-Object round)
            $new = @($samples | Where-Object variant -eq $newName | Sort-Object round)
            $changes = [ordered]@{}
            foreach ($measure in $measures) {
                $values = for ($k = 0; $k -lt $Rounds; ++$k) {
                    if ($base[$k][$measure] -gt 0) { 100 * ($new[$k][$measure] / $base[$k][$measure] - 1) }
                }
                if (@($values).Count -eq $Rounds) { $changes[$measure] = Get-PairedEstimate @($values) }
            }
            $paired["$newName vs $baseName"] = $changes
        }
    }
}
[ordered]@{
    model_sha256 = (Get-FileHash -LiteralPath $modelPath).Hash; rounds = $Rounds; clients = $Clients
    connections_per_client = $ConnectionsPerClient; bursts_per_connection = $BurstsPerConnection
    variants = $variants; medians = $medians; paired_changes = $paired
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'summary.json')
Write-Output "Completed $($samples.Count) trials. Results are in $output."
