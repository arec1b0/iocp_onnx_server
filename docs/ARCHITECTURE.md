# Architecture

This document describes the internal design of the IOCP ONNX Inference Server: its threading model, the lifecycle of a connection, how requests are reassembled from a TCP stream, how inference is executed without extra copies, and the known limitations of the current implementation.

## Component Map

| Component | Files | Responsibility |
|-----------|-------|----------------|
| Entry point | [src/main.cpp](../src/main.cpp) | Loads config and model, starts/stops the server, handles `SIGINT`/`SIGTERM` |
| Server core | [src/server/IocpServer.{h,cpp}](../src/server/IocpServer.cpp) | Listening socket, accept loop, completion port, worker pool, request state machine |
| Connection state | [src/server/Connection.{h,cpp}](../src/server/Connection.cpp) | Per-client socket, read/write I/O contexts, and request framer |
| Inference | [src/inference/OnnxModel.{h,cpp}](../src/inference/OnnxModel.cpp) | ONNX Runtime session, model metadata, zero-copy `Run()` |
| Tensor helpers | [src/inference/TensorUtils.{h,cpp}](../src/inference/TensorUtils.cpp) | Element-count computation from a tensor shape |
| Config | [src/utils/Config.{h,cpp}](../src/utils/Config.cpp) | Server defaults and environment overrides for payload, admission, and deadlines |
| Logging | [src/utils/Logger.{h,cpp}](../src/utils/Logger.cpp) | Thread-safe timestamped logging to stdout/stderr |
| Metrics | [src/utils/Metrics.{h,cpp}](../src/utils/Metrics.cpp) | Singleton JSONL sink for per-inference statistics |

## Threading Model

The server runs **1 + N + 1** threads:

- **Main thread** — after startup, sleeps in a 100 ms polling loop until a termination signal flips the global `g_Running` flag, then calls `IocpServer::Stop()`.
- **Accept thread** (1) — waits for readiness on the nonblocking listener with a finite `select` timeout. It associates accepted sockets with the completion port before admitting them and posting the first read.
- **Worker threads** (N = `Config::workerThreads`, default `std::thread::hardware_concurrency()`) — service I/O completions with `GetQueuedCompletionStatus`. A finite wait lets idle workers observe shutdown after the registry drains. Inference runs directly on these threads outside the connection registry lock.

Because inference happens on the worker thread that dequeued the completion, ONNX Runtime's intra-op parallelism is deliberately set to 1 (`SetIntraOpNumThreads(1)`) — parallelism comes from concurrent requests across workers rather than from parallelizing a single inference.

### Ownership invariants

- The connection registry uniquely owns each admitted connection. The completion key is a borrowed `Connection*`; the registry keeps it and its two I/O contexts alive while a completion can still refer to them.
- Each entry reserves at most one pending operation. The registry lock serializes posting, socket closure, and completion accounting. An immediate posting failure clears the reservation because Winsock will issue no packet for that attempt.
- A worker claims a dequeued completion under the lock and holds exclusive access to the connection's protocol state until it posts the next operation or retires the entry. Shutdown can close that socket while the handler runs, including during inference. The registry holds the connection until that handler returns.
- The listener belongs to the accept thread while it runs. The lifecycle mutex serializes `Start()` and `Stop()`, and socket and port teardown follow the thread joins.
- Admission checks registry capacity under the same lock that inserts and reserves the initial receive. Excess sockets close before connection buffers or IOCP state are allocated. Pending cancellations and active handlers count against the limit until their entries retire.
- The accept thread checks pending deadlines under the registry lock. Timeout closes the socket and retains its entry, buffer, and `OVERLAPPED` until the matching packet is accounted for. A completion for a closed socket retires the entry. Posting, timeout closure, and shutdown closure share the same lock.

### Network deadlines

- **Idle receive.** A receive with no partial request has a fresh idle budget, starting when it is posted. This covers newly admitted clients and persistent clients between requests.
- **Incomplete frame.** The first completion that exposes an incomplete header or payload starts an absolute frame budget. Subsequent fragments keep the same deadline. A partial frame buffered behind a response starts its budget when the final write completion exposes it.
- **Stalled send.** Each pending send has an idle budget. A positive short completion earns a fresh budget for the remaining suffix.

Deadlines use `std::chrono::steady_clock`. Workers compare them at completion accounting, with equality treated as expiration. Time spent in a handler, including inference, has no pending-I/O timer. The existing accept wait also wakes for a registry scan at the configured interval, including during sustained acceptance. Quiet sockets can close up to one scan interval later, plus thread scheduling delay. Smaller intervals increase scan frequency. Each scan is bounded by the configured connection limit.

### Shutdown sequence

1. Publish the stop request under the registry lock. From that point admission and new I/O are closed.
2. Join the accept thread and close the listener. Both the readiness wait and accept call have bounded waits.
3. Close all active sockets to initiate cancellation. Workers account for successful, failed, and canceled completions, and return any entries they hold while processing.
4. Wait for the registry to empty. Idle workers then exit, and the lifecycle caller joins them before closing the completion port and releasing its Winsock reference.

Partial startup failures unwind through the same teardown. Repeated and concurrent `Stop()` calls serialize through the lifecycle mutex. Shutdown closes inference clients promptly and waits for any inference already in progress to return.

## Connection Lifecycle

```
WSAAccept → register Connection → post WSARecv
Read completion → append received bytes → inspect buffered stream
Final write completion ─────────────────→ inspect buffered stream

Inspect buffered stream
  complete frame → run inference → post WSASend
  incomplete frame → post WSARecv
  invalid prefix → retire registry entry

Write completion
  short → repost unsent suffix
  full → inspect buffered stream

Failed or zero-byte completion → retire registry entry
```

Key points:

- Each `Connection` owns a read and a write `IOContext`, each with a `WSAOVERLAPPED`. The registry matches the returned address against the pending context for the connection in the completion key.
- The read buffer is fixed at 64 KiB. A cursor retains bytes after the current frame. Posting a receive compacts any incomplete header to the start of the buffer. The framer accumulates only bytes of a single validated payload.
- At most **one overlapped operation is outstanding per connection at a time**. A short write completion advances the confirmed byte count and reposts the unsent suffix using the same response buffer. The final write completion lets the worker inspect the next frame. This serializes request processing and responses across workers.
- The worker retires the entry after accounting for a terminal completion. A socket closed by shutdown stays registered until its pending operation or active handler is accounted for.

## Request State Machine

The wire protocol ([PROTOCOL.md](PROTOCOL.md)) is a length-prefixed float array. A request may arrive fragmented across many completions, and a completion can contain bytes from more than one request. The per-connection framer processes one request at a time.

1. **Header.** Wait for eight bytes in the read buffer and decode the little-endian `uint64` count. Close the connection if it differs from the validated model input count.
2. **Payload.** Accumulate fragments up to the model's input payload size. Once complete, assemble the `std::vector<float>`, run inference, and post the response. Retain trailing bytes by advancing the read cursor.
3. **Response.** Finish the pending response before inspecting the retained bytes. A complete buffered request starts its response immediately. An incomplete frame re-arms the receive. A malformed subsequent prefix closes the connection.

Each completion advances at most one request. The registry still owns the connection until its pending I/O or active handler is accounted for during disconnect and shutdown.

## Zero-Copy Inference

`OnnxModel::RunInference` avoids ONNX Runtime-managed tensor allocations:

- The input `std::vector<float>` assembled by the framer is wrapped in-place with `Ort::Value::CreateTensor<float>` over CPU memory.
- The output `std::vector<float>` is pre-sized from the model's static output shape and likewise bound in-place, so `session.Run()` writes results directly into the buffer that is then memcpy'd into the send context.

Model metadata is read once at load time (`OnnxModel::Load`):

- Only the **first input node** and **first output node** are used.
- Dynamic dimensions (reported as `-1`) are pinned to `1`, effectively fixing batch size to 1.
- Graph optimization level is `ORT_ENABLE_ALL`.

Model loading computes both element counts with checked multiplication and verifies that both payloads fit the configured limit. The input count is used to validate each request before payload accumulation begins.

## Observability

### Connection metrics

`IocpServer::GetConnectionMetrics()` returns a single consistent snapshot of
the registry gauges and counters. The existing registry lock covers every
update and the copy. Formatting and output run after releasing that lock.
The normal I/O completion path uses its existing lock acquisitions and
performs no telemetry allocation.

- `connections_accepted_total` increments when an accepted socket is associated with IOCP and inserted in the registry. An immediate failure posting its initial receive still leaves one admission followed by one retirement.
- `connections_rejected_capacity_total` increments when a socket is declined at the registry cap during normal admission.
- `connections_closed_total` increments once when a registry entry retires, after its outstanding I/O or handler is accounted for. This includes peer EOF, socket failure, protocol rejection, timeout, and shutdown.
- `connections_active` is the registry size, including connections with a handler in progress or a closed socket awaiting a completion. `connections_high_watermark` is the maximum size reached on insertion.
- `pending_io_operations` counts successful overlapped posts whose matching IOCP packets have yet to be accounted for. A matched packet decrements the gauge for success, failure, EOF, and cancellation. An immediate posting failure leaves it unchanged.
- `idle_timeouts_total`, `incomplete_frame_timeouts_total`, and `send_timeouts_total` count expiration by phase. They increment when a scan closes an open socket, when a successful completion with positive bytes is rejected as expired, or when an expired frame budget prevents another receive from being posted.
- `io_failures_total` counts runtime listener readiness failures, accept failures other than would-block and pre-admission peer resets, socket association and synchronous post failures, and non-cancellation IOCP errors. An unmatched packet or an unexpected dequeue failure also counts once. Readiness and dequeue timeouts, successful zero-byte completions, protocol rejection, and startup errors leave this counter unchanged.
- `cancellation_requests_total` increments when timeout or shutdown closes an open socket with a pending operation. `cancellation_completions_total` increments only when a matched IOCP packet reports `ERROR_OPERATION_ABORTED`. The request can also drain as success or another error, and an aborted completion can occur independently of a server close request.

Cumulative counters are unsigned 64-bit values that saturate. They and the
high-water mark persist across `Start()` and `Stop()` on one server instance.
A new instance starts at zero. A completed stop leaves both gauges at zero.
Before counter saturation, accepted minus closed equals active.

The CLI writes a JSON line with event `connection_metrics` to stdout after
startup, every 30 seconds while running, and after graceful shutdown.
Each record contains the phase, Unix timestamp in milliseconds, and the
aggregate snapshot. The payload and model inputs stay out of these records.

### Inference metrics

`Metrics` is a mutex-guarded singleton appending one JSON line per inference to `metrics.jsonl` (opened in append mode in the process working directory):

| Field | Meaning |
|-------|---------|
| `timestamp` | Unix epoch, milliseconds |
| `latency_ms` | Wall-clock duration of `RunInference` only |
| `input_mean`, `input_stddev` | Input tensor statistics, computed on the worker thread — the drift signal |
| `element_count` | Input tensor size |

The intent is lightweight **model/data drift detection** without external dependencies: a change in the input distribution (e.g. an upstream feature pipeline bug) appears as a step in `input_mean` long before model quality metrics would reveal it. `scripts/analyze_metrics.py` visualizes both series; `scripts/load_test.py` deliberately injects a mid-test mean shift to demonstrate the mechanism.

## Known Limitations & Hardening Notes

Deliberate simplifications to keep the codebase compact. Roughly in order of importance if the server were to face untrusted traffic:

1. **Shutdown waits for current inference.** Socket closure cancels network I/O. A running model call must return before its worker can exit.
2. **Fixed listener and model settings.** Address, port, model path, and worker count are compiled in.
3. **Single model, single node, `float32` only.** Dynamic dimensions are pinned to 1.
