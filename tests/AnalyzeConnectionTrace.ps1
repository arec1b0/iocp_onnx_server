[CmdletBinding()]
param([Parameter(Mandatory)][string] $Directory)

$ErrorActionPreference = 'Stop'
$path = (Resolve-Path -LiteralPath $Directory).Path
$result = Get-Content -LiteralPath (Join-Path $path 'result.json') -Raw | ConvertFrom-Json
$records = @(Get-Content -LiteralPath (Join-Path $path 'connection-trace.jsonl') | ConvertFrom-Json)
$metadata = $records[0]
$events = @($records | Select-Object -Skip 1)
$clients = @(Get-Content -LiteralPath (Join-Path $path 'client-trace.jsonl') | ConvertFrom-Json)
$samples = @(Get-Content -LiteralPath (Join-Path $path 'process-samples.jsonl') | ConvertFrom-Json)
$final = if ($result.final_snapshot) { $result.final_snapshot } else { $result.failure_last_snapshot }
$names = @{
    '64' = 'ERROR_NETNAME_DELETED'; '995' = 'ERROR_OPERATION_ABORTED'; '1236' = 'ERROR_CONNECTION_ABORTED'
    '10049' = 'WSAEADDRNOTAVAIL'; '10053' = 'WSAECONNABORTED'; '10054' = 'WSAECONNRESET'
    '10055' = 'WSAENOBUFS'; '10060' = 'WSAETIMEDOUT'; '10061' = 'WSAECONNREFUSED'
}

function Summarize-Codes($Entries) {
    @($Entries | Where-Object { $null -ne $_.code } | Group-Object operation, outcome, code | ForEach-Object {
        $first = $_.Group[0]
        [ordered]@{ operation = $first.operation; outcome = $first.outcome; code = $first.code
            name = $names[[string]$first.code]; count = $_.Count }
    })
}

$issues = [Collections.Generic.List[string]]::new()
if ($metadata.event -ne 'trace_metadata' -or $metadata.recorded - $metadata.overwritten -ne $events.Count) {
    $issues.Add('Invalid trace metadata.')
}
if ($metadata.overwritten -or $result.client_records_overwritten) { $issues.Add('One or more trace buffers overwrote records.') }
if (@($events | Where-Object { $_.pending -gt $_.active -or $_.active -gt $_.capacity -or
    $_.event -in @('unmatched', 'dequeue_error') }).Count) { $issues.Add('Unexpected registry or completion event.') }
if (-not $final -or $final.connections_active -ne 0 -or $final.pending_io_operations -ne 0 -or
    $final.connections_accepted_total -ne $final.connections_closed_total) { $issues.Add('No verified final drain.') }
if (-not $metadata.overwritten -and $final) {
    $eventCounts = @{ admitted = 'connections_accepted_total'; retired = 'connections_closed_total'
        capacity_reject = 'connections_rejected_capacity_total'; idle_timeout = 'idle_timeouts_total'
        frame_timeout = 'incomplete_frame_timeouts_total'; send_timeout = 'send_timeouts_total' }
    foreach ($event in $eventCounts.Keys) {
        $count = @($events | Where-Object event -eq $event).Count
        if ($count -ne $final.($eventCounts[$event])) { $issues.Add("Trace count mismatch for $event.") }
    }
}

$byTuple = @{}
foreach ($event in $events) {
    if ($event.connection_id -eq 0) { continue }
    $key = "$($event.remote)|$($event.local)"
    if (-not $byTuple.ContainsKey($key)) { $byTuple[$key] = [Collections.Generic.List[object]]::new() }
    $byTuple[$key].Add($event)
}

$failureOutcomes = @('early_eof', 'socket_error', 'harness_deadline', 'harness_error')
$expected = @{ capacity_excess = 'capacity_reject'; idle_probe = 'idle_timeout'; frame_probe = 'frame_timeout' }
$rolesById = @{}
$correlations = foreach ($client in $clients) {
    $timeline = @($byTuple["$($client.local)|$($client.remote)"] | Where-Object {
        $_.accepted_at_us -ge $client.started_us -and $_.accepted_at_us -le $client.ended_us + 100000
    })
    $ids = @($timeline.connection_id | Sort-Object -Unique)
    if ($ids.Count -eq 1) { $rolesById[[string]$ids[0]] = $client.role } else { $timeline = @() }
    if ($client.outcome -notin $failureOutcomes -and -not $expected.ContainsKey($client.role)) { continue }
    $decision = @($timeline | Where-Object event -notin @('admitted', 'retired') | Select-Object -First 1)
    if ($expected.ContainsKey($client.role) -and $decision.event -ne $expected[$client.role]) {
        $issues.Add("The $($client.role) client did not correlate with $($expected[$client.role]).")
    }
    $nearest = $samples | Sort-Object { [Math]::Abs($_.timestamp_us - $client.ended_us) } | Select-Object -First 1
    [ordered]@{ client = $client; matched = $ids.Count -eq 1; server_first_decision = $decision
        server_events = $timeline; nearest_host_sample = $nearest }
}

$counts = [ordered]@{}
foreach ($field in 'TimeWait', 'Established', 'CloseWait', 'target_TimeWait', 'target_Established',
        'target_CloseWait', 'observed_dynamic_ports', 'dynamic_ports_not_seen') {
    $measure = $samples | ForEach-Object { $_.tcp.$field } | Measure-Object -Minimum -Maximum
    $counts[$field] = @($measure.Minimum, $measure.Maximum)
}

$serverErrors = @($events | Where-Object { $_.code -ne 0 } | ForEach-Object {
    [pscustomobject]@{ operation = $_.operation; outcome = $_.phase; code = $_.code
        role = $rolesById[[string]$_.connection_id] ?? 'untracked' }
})
$phases = @($clients | Where-Object phase -gt 0 | Group-Object phase | ForEach-Object {
    $bounds = (@($_.Group.started_us) + @($_.Group.ended_us)) | Measure-Object -Minimum -Maximum
    $seconds = ($bounds.Maximum - $bounds.Minimum) / 1000000
    $verified = ($_.Group.responses_verified | Measure-Object -Sum).Sum
    [ordered]@{ phase = [int]$_.Name; role = $_.Group[0].role; seconds = $seconds
        connections = $_.Count; attempts_per_second = $_.Count / $seconds
        requests = $verified; requests_per_second = $verified / $seconds }
})
$summary = [ordered]@{
    workload = $result.workload
    workload_status = $result.status
    elapsed_seconds = $result.elapsed_seconds
    validated_requests = $result.validated_requests
    client_attempts = $result.client_attempts
    client_closures = $result.client_closures
    attempts_per_second = $result.client_attempts / $result.elapsed_seconds
    closures_per_second = $result.client_closures / $result.elapsed_seconds
    requests_per_second = $result.requests_per_second
    latency_ms = @($result.latency_p50_ms, $result.latency_p95_ms, $result.latency_p99_ms)
    phases = $phases
    tcp_min_max = $counts
    peak_private_bytes = $result.peak_private_bytes
    peak_handles = $result.peak_handles
    cpu_seconds = $result.last_resource_sample.cpu_seconds
    observed_pending_max = ($events.pending | Measure-Object -Maximum).Maximum
    overlap_exit_code = $result.overlap_exit_code
    final = $final
    client_codes = @(Summarize-Codes $clients)
    server_codes = @(Summarize-Codes $serverErrors)
    server_codes_by_role = @($serverErrors | Group-Object role, operation, outcome, code | ForEach-Object {
        $first = $_.Group[0]; [ordered]@{ role = $first.role; operation = $first.operation; phase = $first.outcome
            code = $first.code; count = $_.Count }
    })
    unexpected_client_outcomes = @($clients | Where-Object outcome -in $failureOutcomes).Count
    correlations = @($correlations)
    issues = $issues.ToArray()
}
$summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $path 'correlation.json')
if ($issues.Count) { throw ($issues -join ' ') }
Write-Output (Join-Path $path 'correlation.json')
