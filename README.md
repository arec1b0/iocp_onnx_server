# IOCP ONNX Inference Server

A lightweight, asynchronous C++20 inference server for Windows, built on I/O Completion Ports (IOCP) and the ONNX Runtime C++ API. Designed for direct memory control, minimal latency, and zero-copy tensor execution.

The server accepts raw `float32` tensors over a custom binary TCP protocol, runs them through a single ONNX model, and streams the output tensor back — no HTTP, no JSON, no serialization overhead on the hot path.

## Features

- **IOCP-based networking** — overlapped (asynchronous) TCP sockets multiplexed onto a completion port, serviced by a pool of worker threads matched to hardware concurrency.
- **Zero-copy inference** — input and output buffers are bound directly to ONNX Runtime tensors via `Ort::Value::CreateTensor`, avoiding intermediate tensor allocations.
- **TCP fragmentation handling** — a per-connection accumulation buffer with a header/payload state machine correctly reassembles requests split across arbitrary packet boundaries.
- **Built-in observability** — every inference is recorded to `metrics.jsonl` (latency, input mean/stddev, element count), enabling latency tracking and input data drift detection.
- **Graceful shutdown** — `SIGINT`/`SIGTERM` handling with orderly teardown of the accept thread, worker pool, and completion port.
- **CI pipeline** — GitHub Actions workflow that downloads ONNX Runtime, builds the project with MSVC, and uploads release artifacts.

## Repository Layout

```
iocp_onnx_server/
├── CMakeLists.txt              # Build configuration (MSVC, C++20)
├── src/
│   ├── main.cpp                # Entry point: config, model load, server lifecycle
│   ├── server/
│   │   ├── IocpServer.{h,cpp}  # Listener, accept thread, IOCP worker pool
│   │   └── Connection.{h,cpp}  # Per-client state, I/O contexts, receive buffer
│   ├── inference/
│   │   ├── OnnxModel.{h,cpp}   # ONNX Runtime session wrapper, zero-copy Run()
│   │   └── TensorUtils.{h,cpp} # Tensor shape helpers
│   └── utils/
│       ├── Config.{h,cpp}      # Runtime configuration (defaults)
│       ├── Logger.{h,cpp}      # Thread-safe timestamped console logger
│       └── Metrics.{h,cpp}     # JSONL inference metrics sink
├── scripts/
│   ├── build.bat               # CMake configure + build (Visual Studio 2022, x64)
│   ├── run.bat                 # Launch the built server binary
│   ├── generate_dummy.py       # Export a trivial PyTorch model (y = 2x) to ONNX
│   ├── test_client.py          # Single-request smoke test over the binary protocol
│   ├── load_test.py            # Concurrent load test with fragmentation + data drift
│   └── analyze_metrics.py      # Render metrics.jsonl into metrics_dashboard.png
├── models/                     # Place model.onnx here (see scripts/generate_dummy.py)
├── docs/
│   ├── ARCHITECTURE.md         # Threading model, connection lifecycle, design notes
│   └── PROTOCOL.md             # Binary wire protocol specification
├── .github/workflows/ci.yml    # Windows CI: build + artifact upload
└── pyproject.toml              # Python tooling dependencies (torch, onnx, matplotlib)
```

## Architecture Overview

```
                    ┌─────────────────────────────────────────────┐
                    │                IocpServer                   │
                    │                                             │
 TCP clients ──────►│  AcceptThread ── WSAAccept ── new Connection│
                    │        │                           │        │
                    │        └── associates socket with  │        │
                    │            completion port ────────┘        │
                    │                                             │
                    │  ┌────────── IOCP handle ──────────┐        │
                    │  │  GetQueuedCompletionStatus       │       │
                    │  ▼                                  ▼       │
                    │  WorkerThread 1  ...  WorkerThread N        │
                    │  (N = hardware_concurrency)                 │
                    │        │                                    │
                    │        │ reassemble request (state machine) │
                    │        ▼                                    │
                    │  OnnxModel::RunInference (zero-copy)        │
                    │        │                                    │
                    │        ├──► Metrics (metrics.jsonl)         │
                    │        └──► WSASend response                │
                    └─────────────────────────────────────────────┘
```

Each accepted connection is associated with the completion port. Worker threads block on `GetQueuedCompletionStatus`; when a read completes, they append the bytes to the connection's accumulation buffer, run a header/payload state machine, and — once a full request is buffered — execute inference and post an asynchronous send. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full design, including the connection lifecycle and known limitations.

## Binary Protocol

Requests and responses share the same little-endian layout (see [docs/PROTOCOL.md](docs/PROTOCOL.md) for the full specification):

| Offset | Size          | Type       | Description                        |
|--------|---------------|------------|------------------------------------|
| 0      | 8 bytes       | `uint64`   | `element_count` — number of floats |
| 8      | 4 × N bytes   | `float32[]`| Tensor data, N = `element_count`   |

The request's `element_count` must equal the total element count of the model's input tensor (for the bundled dummy model: 10). The connection stays open — clients may pipeline multiple request/response cycles over one socket.

Minimal Python client:

```python
import socket, struct

data = [1.0] * 10
with socket.socket() as s:
    s.connect(("127.0.0.1", 8080))
    s.sendall(struct.pack("<Q", len(data)) + struct.pack(f"<{len(data)}f", *data))
    n = struct.unpack("<Q", s.recv(8))[0]
    out = struct.unpack(f"<{n}f", s.recv(n * 4))
    print(out)  # (2.0, 2.0, ...) with the dummy model
```

## Getting Started

### Prerequisites

- Windows 10/11, x64
- Visual Studio 2022 (MSVC toolchain, "Desktop development with C++")
- CMake ≥ 3.20
- [ONNX Runtime](https://github.com/microsoft/onnxruntime/releases) v1.17.1 (win-x64), extracted to `C:/onnxruntime` (so that `C:/onnxruntime/include` and `C:/onnxruntime/lib` exist)
- Python ≥ 3.13 with [uv](https://docs.astral.sh/uv/) (only for the helper scripts)

### Build & Run

```bat
:: 1. Install Python tooling (torch, onnx, matplotlib)
uv sync

:: 2. Generate the dummy model (y = 2x, input shape [1, 10])
uv run scripts\generate_dummy.py

:: 3. Build (from a VS Developer Command Prompt)
scripts\build.bat

:: 4. Run — listens on 0.0.0.0:8080
scripts\run.bat
```

To serve your own model, place it at `models/model.onnx`. The server uses the model's **first** input and output nodes; dynamic dimensions (`-1`) are pinned to `1` at load time.

### Configuration

Configuration is currently compiled-in via [Config.cpp](src/utils/Config.cpp):

| Setting        | Default              | Description                                  |
|----------------|----------------------|----------------------------------------------|
| `modelPath`    | `models/model.onnx`  | Path to the ONNX model (relative to CWD)     |
| `ipAddress`    | `0.0.0.0`            | Listen address                               |
| `port`         | `8080`               | Listen port                                  |
| `workerThreads`| `hardware_concurrency` (fallback 2) | IOCP worker pool size          |

### Testing

With the server running:

```bat
:: Single-request smoke test (sends 10 floats, expects each doubled)
uv run scripts\test_client.py

:: Load test: 20 concurrent clients × 50 requests, with deliberate
:: TCP fragmentation and a simulated input-distribution drift
uv run scripts\load_test.py

:: Render metrics.jsonl into metrics_dashboard.png
uv run scripts\analyze_metrics.py
```

## Observability

Every inference appends one JSON line to `metrics.jsonl` in the working directory:

```json
{"timestamp":1751970000000,"latency_ms":0.042,"input_mean":0.51,"input_stddev":0.29,"element_count":10}
```

- `latency_ms` — wall-clock time of `OnnxModel::RunInference` only (excludes network I/O).
- `input_mean` / `input_stddev` — statistics of the input tensor, intended for **data drift detection**: a shift in the input distribution shows up as a step change in the mean.

`scripts/analyze_metrics.py` turns this log into a two-panel dashboard (latency under load + input drift), saved as `metrics_dashboard.png`.

## CI

[.github/workflows/ci.yml](.github/workflows/ci.yml) runs on pushes/PRs to `main`: it provisions ONNX Runtime 1.17.1 on a `windows-2022` runner, builds the Release configuration with CMake/MSVC, and uploads `build/Release/` as a workflow artifact.

## Known Limitations

This is a compact educational/prototyping server, not a hardened production gateway. The most important caveats (detailed in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)):

- The wire header is an unvalidated 64-bit count — a malicious client can request an enormous allocation. Add a sanity cap before exposing to untrusted networks.
- One inference per completion: requests execute directly on IOCP worker threads (intra-op parallelism is set to 1), so throughput scales with worker count, and a slow model blocks its worker.
- Partial `WSASend` completions and multi-request pipelining edge cases are not fully handled.
- Only the model's first input/output node is served; all tensors are `float32`.
- Configuration is compile-time only (no CLI flags or config file yet).

## License

MIT — see [LICENSE](LICENSE).
