# Stage 2.5 deployment workload and storage validation

The shipped server still runs the accepted Stage 2.4 metrics sink and scheduler. These diagnostics use a dedicated loopback client, Release builds, a scratch directory per trial, and the server's existing inference profiling option.

## Open-loop method

- Use a dedicated PowerShell process and an otherwise quiet host. The server binds its configured port 8080. Run one server and one load generator at a time. The client connects only to IPv4 loopback.
- The checked-in defaults use disjoint server and client affinity masks from the validation host. On another host, capture processor groups, core IDs, and efficiency classes via `[ReliabilityHarness]::ReadCpuTopology()`, then choose disjoint masks of the intended sizes. A server mask limits eligible CPUs for all server threads, including IOCP, ONNX Runtime, inference, and logging. It does not reserve cores, cap clocks, or remove OS and interrupt contention. The worker count varies independently of affinity. ONNX Runtime uses the CPU provider and one intra-op thread.
- One dispatcher generates a fixed schedule or seeded exponential interarrival intervals against `Stopwatch`. It groups due frames into writes using 200 µs scheduling quanta and never waits for model responses. Asynchronous receivers verify each reply against a distinct expected frame, in order. Warmup uses half the reference rate. The 110% test immediately reduces offered load to half the reference rate on the same connections.
- The client bounds each burst by its lane's free pipeline slots. The default is 72 lanes with eight slots each. The trial plan is capped at 500,000 offers with at most 48 MB of precomputed frame bytes. Elapsed scheduling, client slot pressure, actual Winsock errors, EOF, invalid responses, timeouts, and followers discarded when an ordered stream times out are recorded separately. A dispatch more than 1 ms late is counted as late. The dispatcher skips offers already older than its configured dispatch deadline (default 5 ms). Socket submission time is measured at the start of the send, including for each frame in a coalesced write. Connections are persistent during load. Abortive test-client cleanup limits accumulation of client-side TIME_WAIT between runs and can contribute to final server I/O failure counters.
- `offers.tsv.gz` contains one row per planned offer. The time columns are relative monotonic ticks with frequency in `manifest.json`. A minus one indicates no dispatch or no observed terminal timestamp. The phase column indexes the trial's phase array from zero. `trials.jsonl` records realized offered, attempted and verified rates, numeric errors, CPU-time deltas, private bytes and handle samples, connection snapshots, job counters, and drain duration. Phase client data are exact. Server scheduler measurements cover the whole process, including warmup and recovery. The summary writes `reconciled-trials.jsonl` with phase admission and rejection rates where the outside-phase outcomes permit it. Otherwise it supplies bounds. A closed stream with several outstanding requests can lose more offers than the number of server job rejections.
- A separate sampler collects process CPU, handles, and private bytes at phase boundaries and roughly once a second. Compare the actual sample timestamps when using short phase windows. An early version measured resources on the dispatcher thread. Its raw traces showed a burst of missed offers at each resource query, and those trials are excluded from server capacity estimates.
- All-offer p50, p95 and p99 use scheduled time to validated response, ranking unsuccessful offers after every success. A quantile is unavailable if it lands among unsuccessful offers. Successful-only dispatch-to-response quantiles exclude scheduler lag and are recorded separately. They include TCP transport, client receive processing, and server work. CPU efficiency uses server CPU time during the measurement window divided by its cohort's successes received within that window. Percentile and efficiency comparisons require the same arrival and instrumentation settings.
- The Stage 2.4 closed-loop rates seed fixed-rate calibration. A tested floor requires at least 99% exact responses, at most 0.5% undispatched, dispatch lag p99 of at most 2 ms, zero server queue rejections, queue high-water below capacity, and last-half verified throughput at least 90% of the first half. If no floor qualifies, the five points use the lowest calibration rate as their reference and are flagged accordingly. The experimental fractions and a short observed floor do not establish maximum sustained capacity. Calibrate for longer on a quiet deployment host.
- The comparison script considers only usable points sharing a model, CPU budget, and arrival mode. The throughput policy picks the highest verified rate satisfying the stability guard. The illustrative tail policy minimizes all-offer p99 with at least 90% of that rate and at most 0.5% unsuccessful offers. The frontier also considers p50, CPU cost, mean queue depth at admission, and the unsuccessful fraction. These thresholds are test assumptions. An absent candidate means the sample cannot support a recommendation under the policy.

The available models are synthetic proxies. The bundled graph contains one Mul. Dense256 and Dense512 contain six square-by-10 MatMuls (about 3.93 and 15.73 million graph multiply-accumulates, respectively), plus Expand, Gather, and Mul. All expose a single float32 `[1,10]` input and output. The client verifies the exact float32 wire format for `y = 2x` on every successful response. The synthetic workloads do not establish capacity for a deployment model or an execution provider with different concurrency characteristics.

## Storage semantics and failure boundary

Inference workers finish ONNX execution before appending their JSONL record. Formatting precedes the metrics mutex. Appending still runs synchronously under that mutex, before response dispatch. As one worker stalls in append, other producers can wait on the mutex, and the bounded admission queue can reject further work. IOCP workers can still handle admission and socket completion. Server shutdown waits for active inference jobs, including their metrics appends, before the main thread flushes and closes the sink.

The diagnostic builder copies the working sources into a scratch directory and hooks exactly the three stream operations in that copy. The probe waits on named Windows events at the chosen operation, or injects a seven-byte partial append or a failing append on an isolated file. Named events establish when an operation has begun. Network requests and shutdown exercise the real server. The indefinitely held test leaves the gate closed after requesting shutdown, observes the finite controller deadline, and terminates only its own diagnostic process. This models the blocking call. It does not induce a physical disk or driver stall.

- An appended record was accepted by the buffered stream. It may still be in user-space buffers.
- A flushed record qualifies under the sink's checked flush and close accounting. Flush hands buffered data to the OS. Neither stream flush nor close establishes durable persistence.
- A detected partial append can leave an incomplete final line. The accepted writer counts the failure, fences subsequent appends as skipped, and reports delivery failure when it closes. Inference can still return an exact result. On orderly shutdown, incomplete delivery yields process exit code 4. After abrupt termination there may be records buffered with no final report.
- An indefinitely blocked append prevents active job completion. A blocked flush or close also prevents process exit. The separate bounded stdout path does not affect either condition. A hard storage-related exit deadline requires isolating the potentially uninterruptible operation or adopting an I/O design with a demonstrated cancellation bound and explicit delivery acknowledgement.

## Reproduction

Run these commands from the repository root in PowerShell 7. Use fresh output directories and the verified ONNX Runtime 1.18.1 Windows x64 package. Set the explicit affinity masks for the validation host.

```powershell
$ort = (Resolve-Path build/stage173-deps/onnxruntime-win-x64).Path
$exe = 'build/stage25-release/Release/iocp_onnx_server.exe'
pwsh -NoProfile -NonInteractive -File tests/OpenLoopStudy.ps1 -Exe $exe `
    -OutputDirectory build/stage25-evidence/matrix-new -Seconds 2 -WarmSeconds 1.5 -RecoverySeconds 2
pwsh -NoProfile -NonInteractive -File tests/SummarizeOpenLoopStudy.ps1 `
    -StudyDirectory build/stage25-evidence/matrix-new
pwsh -NoProfile -NonInteractive -File tests/BuildStorageStallProbe.ps1 -OnnxRuntimeDir $ort `
    -OutputDirectory build/stage25-storage-probe-new
pwsh -NoProfile -NonInteractive -File tests/StorageStallDiagnostics.ps1 `
    -ServerExe build/stage25-storage-probe-new/build/Release/iocp_onnx_server.exe `
    -OutputDirectory build/stage25-evidence/storage-new
```
