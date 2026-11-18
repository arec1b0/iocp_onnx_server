#Requires -Version 7.0
param([Parameter(Mandatory)] [string] $StudyDirectory)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $StudyDirectory).Path
$trials = @(Get-Content -LiteralPath (Join-Path $root 'trials.jsonl') | ConvertFrom-Json)
foreach ($trial in $trials) {
    $phase = $trial.measurement
    $outside = @($trial.phases | Where-Object phase -ne measurement)
    $outsideSuccess = ($outside.succeeded | Measure-Object -Sum).Sum
    $outsideAttempts = ($outside.attempted | Measure-Object -Sum).Sum
    $acceptedMin = [math]::Max($phase.succeeded, $trial.admitted - $outsideAttempts)
    $acceptedMax = [math]::Min($phase.attempted, $trial.admitted - $outsideSuccess)
    $rejectedMin = [math]::Max(0, $trial.rejected - ($outsideAttempts - $outsideSuccess))
    $rejectedMax = [math]::Min($trial.rejected, ($phase.attempted - $phase.succeeded))
    if ($acceptedMin -gt $acceptedMax -or $rejectedMin -gt $rejectedMax) {
        throw "Measurement phase admissions did not reconcile in $($trial.directory)."
    }
    $rates = @{
        accepted_rps = if ($acceptedMin -eq $acceptedMax) { $acceptedMin / $phase.seconds }
        accepted_rps_lower = $acceptedMin / $phase.seconds
        accepted_rps_upper = $acceptedMax / $phase.seconds
        server_rejected_rps = if ($rejectedMin -eq $rejectedMax) { $rejectedMin / $phase.seconds }
        server_rejected_rps_lower = $rejectedMin / $phase.seconds
        server_rejected_rps_upper = $rejectedMax / $phase.seconds
    }
    foreach ($name in $rates.Keys) { $trial | Add-Member -NotePropertyName $name -NotePropertyValue $rates[$name] -Force }
}
$trials | ForEach-Object { $_ | ConvertTo-Json -Depth 10 -Compress } |
    Set-Content -LiteralPath (Join-Path $root 'reconciled-trials.jsonl')
$points = @($trials | Where-Object load_fraction -gt 0)

function Usable($Trial) {
    $m = $Trial.measurement
    $Trial.sustainable -and $null -ne $m.p50_ms -and $null -ne $m.p99_ms -and
        $null -ne $m.cpu_seconds_per_1000 -and $null -ne $Trial.queue_mean_at_admission
}

function Dominates($A, $B) {
    $a = $A.measurement
    $b = $B.measurement
    $q1 = $A.queue_mean_at_admission
    $q2 = $B.queue_mean_at_admission
    $a.success_rps -ge $b.success_rps -and $a.p50_ms -le $b.p50_ms -and $a.p99_ms -le $b.p99_ms -and
        $a.cpu_seconds_per_1000 -le $b.cpu_seconds_per_1000 -and $q1 -le $q2 -and $a.error_pct -le $b.error_pct -and
        ($a.success_rps -gt $b.success_rps -or $a.p50_ms -lt $b.p50_ms -or $a.p99_ms -lt $b.p99_ms -or
            $a.cpu_seconds_per_1000 -lt $b.cpu_seconds_per_1000 -or $q1 -lt $q2 -or $a.error_pct -lt $b.error_pct)
}

function Point($Trial) {
    if (-not $Trial) { return $null }
    $m = $Trial.measurement
    [ordered]@{
        label = $Trial.label; model = $Trial.model; cpus = $Trial.cpus; workers = $Trial.workers
        target_rps = $m.target_rps; offered_rps = $m.offered_rps; verified_rps = $m.success_rps
        accepted_rps_lower = $Trial.accepted_rps_lower; accepted_rps_upper = $Trial.accepted_rps_upper
        rejected_rps_lower = $Trial.server_rejected_rps_lower; rejected_rps_upper = $Trial.server_rejected_rps_upper
        p50_ms = $m.p50_ms; p95_ms = $m.p95_ms; p99_ms = $m.p99_ms; error_pct = $m.error_pct
        cpu_seconds_per_1000 = $m.cpu_seconds_per_1000; client_cpu_cores = $m.client_cpu_cores
        queue_wait_mean_us = $Trial.queue_wait_mean_us; queue_mean_at_admission = $Trial.queue_mean_at_admission
        queue_high_watermark = $Trial.queue_high_watermark; execute_mean_us = $Trial.execute_mean_us
        worker_utilization_pct = $Trial.worker_utilization_pct; directory = $Trial.directory
    }
}

$policies = @(foreach ($group in $points | Group-Object model, cpus, arrival) {
    $eligible = @($group.Group | Where-Object { Usable $_ })
    $max = $eligible | Sort-Object { $_.measurement.success_rps } -Descending | Select-Object -First 1
    $floor = if ($max) { 0.90 * $max.measurement.success_rps }
    $tail = $eligible | Where-Object { $_.measurement.success_rps -ge $floor -and $_.measurement.error_pct -le 0.5 } |
        Sort-Object { $_.measurement.p99_ms } | Select-Object -First 1
    $pareto = @(foreach ($trial in $eligible) {
        if (-not ($eligible | Where-Object { Dominates $_ $trial } | Select-Object -First 1)) { Point $trial }
    })
    [ordered]@{
        model = $group.Group[0].model; cpus = $group.Group[0].cpus; arrival = $group.Group[0].arrival
        total_operating_points = $group.Count; qualifying_points = $eligible.Count
        throughput = Point $max; p99 = Point $tail; illustrative_minimum_verified_rps = $floor
        illustrative_max_error_pct = 0.5; pareto = $pareto
    }
})
$policies | ConvertTo-Json -AsArray -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'policies.json')
$trials | ForEach-Object { [pscustomobject](Point $_) } |
    Export-Csv -LiteralPath (Join-Path $root 'operating-points.csv') -NoTypeInformation
foreach ($policy in $policies) {
    $a = $policy.throughput
    $b = $policy.p99
    [pscustomobject]@{ model = $policy.model; cpus = $policy.cpus; arrival = $policy.arrival
        stable_points = $policy.qualifying_points; throughput_workers = $a.workers; verified_rps = $a.verified_rps
        p99_at_max_ms = $a.p99_ms; tail_workers = $b.workers; tail_verified_rps = $b.verified_rps
        tail_p99_ms = $b.p99_ms }
}
