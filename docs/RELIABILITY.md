# Reliability validation

Run these commands from the repository root in PowerShell 7 on Windows x64. MSVC v143, the Windows SDK, and an extracted x64 ONNX Runtime distribution are required. CMake defaults to `C:/onnxruntime`. Set `-DONNXRUNTIME_DIR=<path>` to use another extraction. The CLI tests and benchmark need port 8080 free. The harness connects only to IPv4 loopback, and the server under test uses its usual listener on all IPv4 interfaces. Run the tests on an isolated test host.

## Release, fault tests, and AddressSanitizer

```powershell
cmake -S . -B build/stage17-release -G 'Visual Studio 17 2022' -A x64
cmake --build build/stage17-release --config Release --parallel 1 -- /nr:false
ctest --test-dir build/stage17-release -C Release --output-on-failure
ctest --test-dir build/stage17-release -C Release -L network --repeat until-fail:3 -V
./tests/ProtocolSmoke.ps1 -ServerExe build/stage17-release/Release/iocp_onnx_server.exe

$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.ASAN -property installationPath
$toolVersion = (Get-Content (Join-Path $vs 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$env:PATH = "$(Join-Path $vs "VC/Tools/MSVC/$toolVersion/bin/Hostx64/x64");$env:PATH"
cmake -S . -B build/stage17-asan -G 'Visual Studio 17 2022' -A x64 `
  '-DCMAKE_CXX_FLAGS=/EHsc /Zi /fsanitize=address' '-DCMAKE_EXE_LINKER_FLAGS=/DEBUG:FULL /INCREMENTAL:NO'
cmake --build build/stage17-asan --config Release --parallel 1 -- /nr:false
ctest --test-dir build/stage17-asan -C Release --output-on-failure
./tests/ProtocolSmoke.ps1 -ServerExe build/stage17-asan/Release/iocp_onnx_server.exe
```

The network CTests use ephemeral ports and real Winsock completions. The lifecycle target injects failure before creating the second worker, after the first worker and the listener are live. The same startup error handler rolls back both injected and real worker-launch failures. The policy tests can drive deadline transitions with a manual monotonic clock. The shutdown-during-inference case uses a gated test inference callback. The TCP smoke suite uses native process termination for the client and verifies the server can still serve exact responses. Run churn repeats in small groups and run them separately from the soak. Each lifecycle iteration opens over 1,600 loopback connections, and rapid repeats can exhaust the Windows dynamic TCP port pool while closed sockets remain in `TIME_WAIT`.

Run MSVC static analysis with the concurrency extension in a dedicated PowerShell process so the environment flags apply only to that build.

```powershell
$env:_CL_ = '/analyze /analyze:plugin EspXEngine.dll'
${env:Esp.Extensions} = 'ConcurrencyCheck.dll'
cmake -S . -B build/stage17-analysis -G 'Visual Studio 17 2022' -A x64 '-DCMAKE_CXX_FLAGS=/EHsc'
cmake --build build/stage17-analysis --config Release --parallel 1 -- /nr:false 2>&1 |
  Tee-Object build/stage17-analysis/msvc-analysis.log
```

AddressSanitizer checks instrumented memory accesses. MSVC's concurrency extension flags selected locking patterns. These diagnostics leave general data races outside the verification scope. Application Verifier and Page Heap require a separately configured diagnostic session with a debugger, including cleanup of per-image settings after the run.

## Mixed-client soak

```powershell
$server = 'build/stage17-release/Release/iocp_onnx_server.exe'
./tests/ReliabilitySoak.ps1 -ServerExe $server -Smoke -OutputDirectory build/soak-smoke
./tests/ReliabilitySoak.ps1 -ServerExe $server -OutputDirectory build/soak-60m
Get-Content build/soak-60m/result.json -Raw
```

Use a fresh output directory for each run. The default is 3,600 seconds with seed 17381 and a limit of 16 registered connections. `-Smoke` defaults to 45 seconds. Override `-DurationSeconds`, `-Seed`, and `-Capacity` as needed. `-Mix` accepts worker counts for `persistent`, `short`, `fragmented`, `idle`, `slow`, `stalled`, `reset`, and `saturation`. The default has two persistent workers and one worker per other role. The harness coordinates deliberate saturation bursts with the healthy clients so an expected capacity rejection cannot hide a response failure. Seeds reproduce each client's inputs and timing choices. OS scheduling and completion order vary between runs.

The generated ONNX fixture doubles each of the ten input floats and repeats the vector in a static 5 MiB output with the default `-OutputRows`. The harness calculates the expected response independently and compares every byte of every healthy response. `-RequestGapMs` controls request pacing, `-SaturationIntervalMs` controls the interval between bursts, and `-OperationTimeoutMs` bounds client I/O. The test sets idle and stalled-send deadlines to 1,200 ms, the incomplete-frame deadline to 600 ms, and the timeout check interval to 25 ms. Idle and incomplete clients wait for peer closure. Backpressured clients send several requests with a small receive buffer and leave the responses unread. Trim or omit roles through `-Mix` for focused runs.

The evidence directory contains the workload parameters, stdout and stderr, validated connection snapshots, private bytes and handle samples, cumulative CPU seconds, and `result.json`. The latency percentiles describe end-to-end healthy responses in 1 ms histogram bins. Startup, periodic, and shutdown snapshots must balance `accepted - closed == active` and satisfy `pending <= active <= high-water <= capacity`. Both gauges must be zero after a real console interrupt and a zero exit status. The controller retains sockets across the shutdown signal. Capacity rejections close excess sockets while the registered count stays bounded. Cancellation requests are recorded separately from completion packets whose exact error is `ERROR_OPERATION_ABORTED`. A zero cancellation-completion count can coexist with a fully drained registry.

The harness directs stdout to a regular file. This soak measures shutdown with a writable destination. Memory samples measure the server process's private commit, and CPU seconds are summed across its threads.

## Standard output backpressure

The former logger serialized `std::cout`, its JSON flush, and `std::cerr` under one mutex. `std::cerr` could also flush its tied stdout stream. A blocked write could prevent the main thread from reaching `Stop()`, hold a worker needed for the registry drain, or stall final JSON after networking had stopped. The application now routes its standard-stream diagnostics, including the ONNX Runtime callback, through one logger. Inference statistics still use the separate `metrics.jsonl` file.

For dedicated local Windows stdout and stderr pipes, the shutdown test requires exit within two seconds of CTRL+C with the reader paused. The logger verifies the inherited synchronous handle mode, enables `PIPE_NOWAIT`, and issues no wait for a pipe reader. It counts a record as written after Windows accepts every byte, including the newline. The application stops and drains the server before producing the final snapshot. The logger finishes after the server and model have left scope. It holds no connection references and starts no background thread or pending overlapped write. Logger finalization is idempotent. The server process still depends on the OS, a returning in-flight model call, and available storage for inference statistics. Direct console and regular-file output retain synchronous local write semantics.

Each distinct sink retains at most one partial line in a 4,096-byte buffer. Other submissions get one attempt to advance that suffix before offering a new record. A full pipe with no progress causes a drop or leaves the partial line for the next submission. If a pipe is already busy when opening the sink, the logger drops that record and retries enabling non-waiting mode on the next submission. Readers should discard any final bytes without a newline at EOF. The logger writes no further record behind an incomplete prefix. Oversize text has a truncation marker and oversize JSON is dropped. Stdout and stderr sharing one Windows handle object share the same buffer. A supervisor should give each pipe write endpoint to a single logging process, since non-waiting mode also applies to inherited duplicates of that handle.

If stdout cannot publish the final connection metrics, the logger attempts the full record on a separate stderr sink. If an error diagnostic cannot start on stderr, the logger tries stdout. At exit a best-effort `log_delivery` JSON record reports prior output attempts and final snapshot delivery. Dropped records and trailing partial records are counted separately. Counters remain accurate in the server registry irrespective of delivery. If both sinks prevent a complete loss report, exit code `2` signals that networking shut down with unreported log loss. Exit code `3` signals unreported loss alongside application startup failure. A healthy drain with successful delivery or a complete loss notice retains exit code `0`. The normal startup-failure code is `1`. Delivery under backpressure is best effort so shutdown can proceed with the reader paused.

The stdout regression fills a dedicated pipe without altering its wait mode or leaving a test writer pending. It sends a real console interrupt with a live client and holds the reader paused through exit or deadline. It verifies the final drained metrics and loss notice through stderr. Each iteration starts a new server process. Run the regression in its own PowerShell process with port 8080 free.

```powershell
ctest --test-dir build/stage17-release -C Release -L logging --repeat until-fail:3 -V
pwsh -NoProfile -NonInteractive -File tests/StdoutBackpressure.ps1 `
  -ServerExe build/stage17-release/Release/iocp_onnx_server.exe `
  -OutputDirectory build/stdout-backpressure
```

The logging CTests use real Windows pipes. The slow-reader and broken-reader cases inject a short native write and a period without progress in the test build to exercise the production short-write path deterministically. The pressure test submits 200,000 records to a full pipe before racing further submissions with finalization. The release server and the process-level regression use the native write sizes.

## Paired Stage 1.5 and Stage 1.6 benchmark

Build the committed Stage 1.5 source in a separate directory without changing the working tree. Use the same MSVC toolset and ONNX Runtime for both builds. Run after the soak and other tests finish so they do not compete for port 8080 or processor time.

```powershell
git archive --format=zip -o build/stage15-source.zip f17f317
Expand-Archive -LiteralPath build/stage15-source.zip -DestinationPath build/stage15-source
cmake -S build/stage15-source -B build/stage17-baseline -G 'Visual Studio 17 2022' -A x64
cmake --build build/stage17-baseline --config Release --target iocp_onnx_server --parallel 1 -- /nr:false
./tests/ConnectionMetricsBenchmark.ps1 `
  -BaselineExe build/stage17-baseline/Release/iocp_onnx_server.exe `
  -CandidateExe build/stage17-release/Release/iocp_onnx_server.exe `
  -Pairs 15 | Tee-Object build/stage17-benchmark.jsonl
```

The benchmark uses the shipped model. Every trial starts a fresh server, warms up the same workload, and validates the ordered response bytes. Each pair runs both variants, with order alternating between pairs. The JSONL has raw trial throughput and request latency percentiles, variant medians, and paired median changes with 95% percentile bootstrap intervals from 10,000 resamples of the pairs. Request latency is measured from the start of the burst write through receipt of the complete response frame. Read coalescing can give multiple frames the same observed completion timestamp. The CTest lifecycle log also reports latency of `GetConnectionMetrics()` with and without active connections. These timings include the snapshot copy and mutex wait, plus scheduling and timer effects. They do not apportion handler-side contention or include formatting and writing the JSON line.

## Host churn investigation

Run one experiment at a time with port 8080 free. Experiment C is the only case that starts a competing workload. It overlaps the original 45-second mixed smoke with three iterations of the existing ephemeral-loopback lifecycle suite. Experiments B, D, and E use the shipped model and 100 verified requests per phase. B increases the paced connection rate from 5 to 20 to 40 per second. D uses 5 per second throughout. E reuses one connection per phase at B's request pacing. F fills eight registered slots, checks one capacity rejection, verifies the holders still get exact replies, and then checks idle and incomplete-frame expiration.

```powershell
netsh int ipv4 show dynamicport tcp
netsh interface ipv4 show excludedportrange protocol=tcp
$build = 'build/stage172-release'
$server = "$build/Release/iocp_onnx_server.exe"
$root = "build/churn-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
New-Item -ItemType Directory $root | Out-Null
./tests/ProtocolSmoke.ps1 -ServerExe $server | Tee-Object "$root/A-smoke.log"
./tests/ReliabilitySoak.ps1 -ServerExe $server -OutputDirectory "$root/B" -ConnectionDiagnostics -ConnectionRates 5,20,40
./tests/ReliabilitySoak.ps1 -ServerExe $server -OutputDirectory "$root/C" -ConnectionDiagnostics -Smoke -OverlapBuild $build
./tests/ReliabilitySoak.ps1 -ServerExe $server -OutputDirectory "$root/D" -ConnectionDiagnostics -ConnectionRates 5,5,5
./tests/ReliabilitySoak.ps1 -ServerExe $server -OutputDirectory "$root/E" -ConnectionDiagnostics -ConnectionRates 5,20,40 -ReuseConnections
./tests/ReliabilitySoak.ps1 -ServerExe $server -OutputDirectory "$root/F" -ConnectionDiagnostics -ProbePolicies -Capacity 8
foreach ($case in 'B', 'C', 'D', 'E', 'F') {
    ./tests/AnalyzeConnectionTrace.ps1 -Directory "$root/$case"
}
```

Pass `-DynamicPortStart` and `-DynamicPortCount` if the host reports a range other than `49152` plus `16384` ports. `dynamic_ports_not_seen` is the configured range minus distinct local ports visible in sampled IPv4 TCP rows. The estimate includes ports the allocator might exclude and omits reservations or uses absent from the table. `TIME_WAIT` or a large socket count alone does not establish allocation failure. The experiments leave the host's TCP configuration intact.

`-ConnectionDiagnostics` enables `IOCP_ONNX_CONNECTION_TRACE=1` for the child server. The server keeps the most recent 32,768 admission, timeout, error, terminal-completion, and retirement records in memory. It writes `connection-trace.jsonl` after stopping. Client traces retain 8,192 connection records. Both sides include UTC microseconds and endpoints. The analyzer joins on the reversed socket tuple and overlapping connection lifetime and leaves ambiguous matches unresolved. It checks complete trace counts against final metrics and records the closest sampled TCP state beside a correlated failure. Winsock submission errors and Win32 IOCP completion errors retain their distinct API phases. A successful receive of zero bytes is EOF and has code zero. A harness deadline without a Winsock status has a null code. Diagnostic files contain endpoint metadata and count fields. They contain no request or response payloads. Tracing adds measurement overhead and a post-drain file write, so leave it disabled for ordinary service and throughput baselines.
