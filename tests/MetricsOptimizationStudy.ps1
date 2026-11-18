[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $BaselineExe,
    [Parameter(Mandatory)][string] $OptimizedExe,
    [Parameter(Mandatory)][string] $BypassExe,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $Dense256 = 'build/stage22-dense.onnx',
    [string] $Dense512 = 'build/stage23-evidence/dense512.onnx',
    [uint64] $Cpu2Mask = 6,
    [uint64] $Cpu4Mask = 30,
    [uint64] $Cpu8Mask = 510,
    [uint64] $ClientMask = 15873,
    [ValidateRange(15, 63)][int] $Rounds = 15,
    [ValidateRange(1, 15)][int] $ProfileRounds = 3,
    [ValidateSet('All', 'Comparison', 'Workers', 'Profiles', 'Summary')][string] $Part = 'All'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BenchmarkStatistics.ps1')
$root = (New-Item -ItemType Directory -Force -Path $OutputDirectory).FullName
$benchmark = Join-Path $PSScriptRoot 'InferenceSchedulerBenchmark.ps1'
$workloads = @(
    @{Name='bundled'; Model=(Join-Path $PSScriptRoot '../models/model.onnx'); Bursts=48},
    @{Name='dense256'; Model=$Dense256; Bursts=24},
    @{Name='dense512'; Model=$Dense512; Bursts=8}
)
$budgets = [ordered]@{'2'=$Cpu2Mask; '4'=$Cpu4Mask; '8'=$Cpu8Mask}
foreach ($cpu in $budgets.Keys) {
    if ([Numerics.BitOperations]::PopCount([uint64]$budgets[$cpu]) -ne $cpu) {
        throw "The $cpu-processor mask selects a different number of processors."
    }
}

function Confirm-Run([int] $Code, [string] $Directory) {
    if ($Code -ne 0) {
        if (Test-Path -LiteralPath "$Directory/failure.txt") { Get-Content -LiteralPath "$Directory/failure.txt" }
        throw "Benchmark failed in $Directory with exit status $Code."
    }
    Write-Output "Completed $Directory"
}

if ($Part -in 'All', 'Comparison') {
    foreach ($workload in $workloads) {
        $directory = "$root/comparison-$($workload.Name)"
        pwsh -NoProfile -NonInteractive -File $benchmark -Mode MetricsOptimization `
            -ReferenceExe $BaselineExe -CandidateExe $OptimizedExe -BypassExe $BypassExe `
            -Model $workload.Model -OutputDirectory $directory -ServerAffinityMask $Cpu4Mask -ClientAffinityMask $ClientMask `
            -Rounds $Rounds -ConnectionsPerClient 2 -BurstsPerConnection $workload.Bursts -Workers 8
        Confirm-Run $LASTEXITCODE $directory
    }
}

function Invoke-WorkerMatrix([bool] $Profiled) {
    $label = if ($Profiled) { 'profiles' } else { 'workers' }
    $repeats = if ($Profiled) { $ProfileRounds } else { $Rounds }
    $unprofiled = @(if (-not $Profiled) { '-UnprofiledSweep' })
    foreach ($cpu in $budgets.Keys) {
        foreach ($workload in $workloads) {
            $directory = "$root/$label-$($workload.Name)-cpu$cpu"
            pwsh -NoProfile -NonInteractive -File $benchmark -Mode Sweep -StandardWorkerSweep @unprofiled `
                -CandidateExe $OptimizedExe -Model $workload.Model -OutputDirectory $directory `
                -ServerAffinityMask $budgets[$cpu] -ClientAffinityMask $ClientMask -Rounds $repeats `
                -ConnectionsPerClient 2 -BurstsPerConnection $workload.Bursts -RequireMetricsRecords
            Confirm-Run $LASTEXITCODE $directory
        }
    }
}

if ($Part -in 'All', 'Workers') { Invoke-WorkerMatrix $false }
if ($Part -in 'All', 'Profiles') { Invoke-WorkerMatrix $true }

function PhaseMean($Trials, [string] $Phase) {
    Get-Median @($Trials | ForEach-Object {
        $timing = $_.profile.timings[$Phase]
        if ($timing['count'] -le 0) { throw "Missing samples for $Phase." }
        $timing.total_ns / $timing['count'] / 1000
    })
}

function RegistryMean($Trials, [string] $Type) {
    Get-Median @($Trials | ForEach-Object {
        $total = 0
        foreach ($phase in 'completion','handoff','claim','dispatch','submit_result','accept','sweep') {
            $total += $_.profile.timings["${phase}_$Type"].total_ns
        }
        $total / $_.profile.finished / 1000
    })
}

function Dominates($Other, $Row) {
    ($Other.rps -ge $Row.rps -and $Other.p50_ms -le $Row.p50_ms -and
        $Other.p99_ms -le $Row.p99_ms -and $Other.cpu_seconds_per_1000 -le $Row.cpu_seconds_per_1000) -and
        ($Other.rps -gt $Row.rps -or $Other.p50_ms -lt $Row.p50_ms -or
        $Other.p99_ms -lt $Row.p99_ms -or $Other.cpu_seconds_per_1000 -lt $Row.cpu_seconds_per_1000)
}

if ($Part -in 'All', 'Summary') {
    $rows = @()
    $recommendations = foreach ($workload in $workloads) {
        foreach ($cpu in $budgets.Keys) {
            $base = "$($workload.Name)-cpu$cpu"
            $timed = @(Get-Content -LiteralPath "$root/workers-$base/trials.jsonl" | ConvertFrom-Json -AsHashtable)
            $profiled = @(Get-Content -LiteralPath "$root/profiles-$base/trials.jsonl" | ConvertFrom-Json -AsHashtable)
            $cell = foreach ($workers in 1, 2, 4, 8) {
                $variant = "w$workers-q64"
                $t = @($timed | Where-Object variant -eq $variant)
                $p = @($profiled | Where-Object variant -eq $variant)
                $peak = ($p | ForEach-Object {$_.profile.queue_high_watermark} | Measure-Object -Maximum).Maximum
                if ($t.Count -ne $Rounds -or $p.Count -ne $ProfileRounds -or $peak -ge 64 -or
                    ($p | Where-Object {$_.profile.rejected -or $_.profile.cancelled})) {
                    throw "Missing trials or queue pressure in $base/$variant."
                }
                [pscustomobject]@{
                    model=$workload.Name; cpus=[int]$cpu; workers=$workers
                    rps=Get-Median $t.requests_per_second; p50_ms=Get-Median $t.p50_ms
                    p95_ms=Get-Median $t.p95_ms; p99_ms=Get-Median $t.p99_ms
                    cpu_seconds_per_1000=Get-Median $t.cpu_seconds_per_1000
                    rps_min=($t.requests_per_second | Measure-Object -Minimum).Minimum
                    rps_max=($t.requests_per_second | Measure-Object -Maximum).Maximum
                    queue_wait_us=PhaseMean $p 'queue_wait'
                    queue_depth=Get-Median @($p | ForEach-Object {$_.profile.queue_depth_sum_at_admission/$_.profile.admitted})
                    queue_high_watermark=$peak
                    worker_busy_pct=Get-Median @($p | ForEach-Object {100*$_.profile.timings.worker_busy.total_ns/$_.profile.timings.worker_lifetime.total_ns})
                    registry_wait_us_per_job=RegistryMean $p 'wait'; registry_hold_us_per_job=RegistryMean $p 'hold'
                    metrics_format_us=PhaseMean $p 'metrics_format'; metrics_wait_us=PhaseMean $p 'metrics_lock_wait'
                    metrics_append_hold_us=PhaseMean $p 'metrics_append_hold'; pareto=$false
                }
            }
            foreach ($row in $cell) { $row.pareto = -not ($cell | Where-Object { Dominates $_ $row }) }
            $rows += $cell
            $fast = $cell | Sort-Object -Property @{Expression='rps'; Descending=$true}, p99_ms, workers | Select-Object -First 1
            $floor = 0.95 * $fast.rps
            $tail = $cell | Where-Object rps -GE $floor | Sort-Object p99_ms, p50_ms, workers | Select-Object -First 1
            [pscustomobject]@{
                model=$workload.Name; cpus=[int]$cpu; throughput_workers=$fast.workers
                throughput_rps=$fast.rps; throughput_p99_ms=$fast.p99_ms
                illustrative_min_rps=$floor; tail_workers=$tail.workers; tail_rps=$tail.rps; tail_p99_ms=$tail.p99_ms
                pareto_workers=($cell | Where-Object pareto | ForEach-Object workers) -join ','
            }
        }
    }
    $rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$root/worker-results.json"
    $recommendations | Export-Csv -NoTypeInformation -LiteralPath "$root/worker-recommendations.csv"
}
