# Architecture

This document describes the internal design of the IOCP ONNX Inference Server: its threading model, the lifecycle of a connection, how requests are reassembled from a TCP stream, how inference is executed without extra copies, and the known limitations of the current implementation.

## Component Map

| Component | Files | Responsibility |
|-----------|-------|----------------|
| Entry point | [src/main.cpp](../src/main.cpp) | Loads config and model, starts/stops the server, handles `SIGINT`/`SIGTERM` |
| Server core | [src/server/IocpServer.{h,cpp}](../src/server/IocpServer.cpp) | Listening socket, accept loop, completion port, worker pool, request state machine |
| Connection state | [src/server/Connection.{h,cpp}](../src/server/Connection.cpp) | Per-client socket, read/write I/O contexts, receive accumulation buffer |
| Inference | [src/inference/OnnxModel.{h,cpp}](../src/inference/OnnxModel.cpp) | ONNX Runtime session, model metadata, zero-copy `Run()` |
| Tensor helpers | [src/inference/TensorUtils.{h,cpp}](../src/inference/TensorUtils.cpp) | Element-count computation from a tensor shape |
| Config | [src/utils/Config.{h,cpp}](../src/utils/Config.cpp) | Compile-time defaults (model path, bind address, worker count) |
| Logging | [src/utils/Logger.{h,cpp}](../src/utils/Logger.cpp) | Thread-safe timestamped logging to stdout/stderr |
| Metrics | [src/utils/Metrics.{h,cpp}](../src/utils/Metrics.cpp) | Singleton JSONL sink for per-inference statistics |

## Threading Model

The server runs **1 + N + 1** threads:

- **Main thread** — after startup, sleeps in a 100 ms polling loop until a termination signal flips the global `g_Running` flag, then calls `IocpServer::Stop()`.
- **Accept thread** (1) — blocks in `WSAAccept` on the listening socket. For each new client it allocates a `Connection`, associates the client socket with the completion port (using the `Connection*` as the completion key), and posts the first overlapped `WSARecv`.
- **Worker threads** (N = `Config::workerThreads`, default `std::thread::hardware_concurrency()`) — each blocks in `GetQueuedCompletionStatus(INFINITE)` and services I/O completions. **Inference runs directly on these threads**; there is no separate inference queue.

Because inference happens on the worker thread that dequeued the completion, ONNX Runtime's intra-op parallelism is deliberately set to 1 (`SetIntraOpNumThreads(1)`) — parallelism comes from concurrent requests across workers rather than from parallelizing a single inference.

### Shutdown sequence

`IocpServer::Stop()`:

1. Clears `isRunning_` and closes the listening socket — this unblocks `WSAAccept` with an error, letting the accept thread exit its loop.
2. Posts N sentinel completion packets (`PostQueuedCompletionStatus(iocp, 0, 0, NULL)`); each worker recognizes the all-zero packet and breaks out of its loop.
3. Joins the accept thread and all workers, closes the completion port, and calls `WSACleanup()`.

Live client connections are not individually torn down at shutdown; the process exit releases them.

## Connection Lifecycle

```
WSAAccept
   │
   ▼
new Connection ── CreateIoCompletionPort(socket, iocp, key = Connection*)
   │
   ▼
WSARecv (overlapped) ◄────────────────────────────┐
   │                                              │
   ▼  completion (Read)                           │
append to rxBuffer                                │
   │                                              │
   ▼                                              │
state machine:                                    │
  ReadHeader  ── 8 bytes available? ── no ──► re-arm WSARecv
  ReadPayload ── count*4 bytes available? ─ no ─► re-arm WSARecv
   │ yes                                          │
   ▼                                              │
RunInference ──► Metrics::RecordInference         │
   │                                              │
   ▼                                              │
WSASend (overlapped)                              │
   │                                              │
   ▼  completion (Write)                          │
re-arm WSARecv ───────────────────────────────────┘

(any failed/zero-byte completion ──► delete Connection ──► closesocket)
```

Key points:

- Each `Connection` owns two fixed `IOContext` objects (one for reads, one for writes), each embedding a `WSAOVERLAPPED` as its **first member** so the `LPOVERLAPPED` returned by `GetQueuedCompletionStatus` can be cast back to the `IOContext`. The `Connection*` itself travels as the completion key.
- I/O buffers are 64 KiB per context; the receive accumulation buffer (`rxBuffer`) reserves 128 KiB.
- At most **one overlapped operation is outstanding per connection at a time**: a read is only re-armed after a request is either incomplete (need more bytes) or fully answered (write completion arrives). This keeps the state machine single-threaded per connection without locks.
- Connection teardown is driven by completion status: a failed `GetQueuedCompletionStatus` result or a zero-byte transfer (peer closed) deletes the `Connection`, whose destructor closes the socket.

## Request State Machine

The wire protocol ([PROTOCOL.md](PROTOCOL.md)) is a length-prefixed float array. Because TCP is a byte stream, a request may arrive fragmented across many completions, or several requests may arrive in a single completion. The worker handles both with a per-connection state machine over `rxBuffer`:

1. **`ReadHeader`** — wait until ≥ 8 bytes are buffered, then consume the little-endian `uint64` element count and switch to `ReadPayload`.
2. **`ReadPayload`** — wait until `element_count × 4` bytes are buffered, then copy them into a contiguous `std::vector<float>`, run inference, send the response, and switch back to `ReadHeader`.

If the buffer doesn't yet hold a complete header or payload, the worker simply re-arms `WSARecv` and returns to the pool — no busy waiting.

> Note: after a response is sent, processing of `rxBuffer` pauses until the write completion re-arms a read. Bytes of a subsequent pipelined request already sitting in `rxBuffer` are therefore processed only after the next read completion arrives (see Limitations).

## Zero-Copy Inference

`OnnxModel::RunInference` avoids ONNX Runtime-managed tensor allocations:

- The input `std::vector<float>` (assembled from `rxBuffer`) is wrapped in-place with `Ort::Value::CreateTensor<float>` over CPU memory.
- The output `std::vector<float>` is pre-sized from the model's static output shape and likewise bound in-place, so `session.Run()` writes results directly into the buffer that is then memcpy'd into the send context.

Model metadata is read once at load time (`OnnxModel::Load`):

- Only the **first input node** and **first output node** are used.
- Dynamic dimensions (reported as `-1`) are pinned to `1`, effectively fixing batch size to 1.
- Graph optimization level is `ORT_ENABLE_ALL`.

A request whose element count doesn't match the model's expected input size is rejected inside `RunInference` (logged, returns an empty vector); the server still sends a response with `element_count = 0`.

## Observability

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

1. **Unvalidated header** — `expectedElements` comes straight off the wire with no upper bound; a hostile 8-byte header can trigger a multi-gigabyte allocation attempt. A sanity cap (e.g. reject counts above the model's input size) is the first thing to add.
2. **Partial sends unhandled** — the code assumes `WSASend` eventually transmits the whole buffer in one overlapped operation; a short write would corrupt the response stream for that client.
3. **Pipelining latency** — a complete second request already in `rxBuffer` isn't processed until another read completion fires (the post-write path re-arms a read instead of re-running the state machine).
4. **`isRunning_` is a plain `bool`** shared across threads (benign in practice for shutdown signaling, but formally a data race; `std::atomic<bool>` would be correct).
5. **`rxBuffer` front-erase is O(n)** — fine at these message sizes; a ring buffer or read offset would remove the cost.
6. **Error-path cleanup** — `delete conn` on I/O failure relies on no other operation being outstanding for that connection; the single-outstanding-op invariant makes this safe today but fragile under modification.
7. **Fixed configuration** — address, port, model path, and worker count are compiled in; there are no CLI flags, env vars, or config file.
8. **Single model, single node, `float32` only** — no multi-model routing, no non-float dtypes, batch pinned to 1.
9. **`size_t` in the protocol spec** — the wire format is de facto `uint64` little-endian (x64 Windows); clients on other ABIs must not use their native `size_t`.
