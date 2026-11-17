# Binary Wire Protocol

The server speaks a minimal length-prefixed binary protocol over raw TCP. There is no handshake, no framing beyond the length prefix, and no metadata — the client is expected to know the model's input contract out of band.

- **Transport:** TCP, default port `8080`
- **Byte order:** little-endian (x64 native)
- **Element type:** IEEE-754 `float32` only
- **Connection model:** persistent, subject to the configured connection limit and network deadlines

## Request

```
┌──────────────────────┬────────────────────────────────────┐
│ element_count        │ payload                            │
│ uint64 LE (8 bytes)  │ element_count × float32 LE (4 B ea)│
└──────────────────────┴────────────────────────────────────┘
```

`element_count` must equal the total element count of the model's input tensor — the product of all input-shape dimensions after dynamic dims are pinned to 1 (for the bundled dummy model with shape `[1, 10]`: **10**).

The server caps each input and output payload at 16 MiB by default. Set
`IOCP_ONNX_MAX_PAYLOAD_BYTES` to a decimal integer from 4 through 4294967287 (up
to ten digits) before starting the server to change the limit. The 8-byte
prefix is excluded. Startup fails if the override is invalid or either model
tensor exceeds the configured limit.

## Response

Identical layout:

```
┌──────────────────────┬────────────────────────────────────┐
│ element_count        │ payload                            │
│ uint64 LE (8 bytes)  │ element_count × float32 LE (4 B ea)│
└──────────────────────┴────────────────────────────────────┘
```

`element_count` is the product of the model's output-shape dimensions. Exactly one response is sent per complete request, in request order.

Clients can send consecutive requests on the same connection before reading
responses. The server sends one response at a time and checks for the next
buffered request after each full send completion.

### Error signaling

There are no status codes. A prefix whose count differs from the model's
input count closes the connection as soon as the complete prefix arrives.
The server discards any incomplete frame when the peer disconnects. If
inference fails after receiving a valid frame, the server sends
`element_count = 0` with an empty payload. Clients should treat a zero-length
response as an inference error.

The server closes sockets above the admission limit and connections that
expire while idle, receiving a partial frame, or sending a response. The
client may receive a response prefix already sent before a stalled send expired.
See [network deadlines](ARCHITECTURE.md#network-deadlines) for the timing rules.

## Client requirements

1. **Read until complete.** TCP gives no message boundaries: loop on `recv` until all 8 header bytes, then all `element_count × 4` payload bytes have arrived. (The server does the same on its side — requests may be fragmented arbitrarily.)
2. **Use fixed-width types.** The length prefix is always 8 bytes (`uint64`), regardless of the client platform's `size_t`.
3. **Send floats in little-endian.** No byte-order conversion is performed server-side.

## Reference implementation (Python)

```python
import socket
import struct

def infer(host: str, port: int, values: list[float]) -> list[float]:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((host, port))

        # Request: uint64 LE count + float32 LE payload
        s.sendall(struct.pack("<Q", len(values)) +
                  struct.pack(f"<{len(values)}f", *values))

        # Response header: read exactly 8 bytes
        header = b""
        while len(header) < 8:
            chunk = s.recv(8 - len(header))
            if not chunk:
                raise ConnectionError("connection closed mid-header")
            header += chunk
        count = struct.unpack("<Q", header)[0]
        if count == 0:
            raise RuntimeError("server reported inference error")

        # Response payload: read exactly count * 4 bytes
        payload = b""
        while len(payload) < count * 4:
            chunk = s.recv(count * 4 - len(payload))
            if not chunk:
                raise ConnectionError("connection closed mid-payload")
            payload += chunk
        return list(struct.unpack(f"<{count}f", payload))

print(infer("127.0.0.1", 8080, [1.0] * 10))  # dummy model → [2.0] * 10
```

See also [scripts/test_client.py](../scripts/test_client.py) (smoke test) and [scripts/load_test.py](../scripts/load_test.py) (concurrent clients with deliberate fragmentation).
