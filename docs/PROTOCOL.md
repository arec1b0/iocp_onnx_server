# Binary Wire Protocol

The server speaks a minimal length-prefixed binary protocol over raw TCP. There is no handshake, no framing beyond the length prefix, and no metadata — the client is expected to know the model's input contract out of band.

- **Transport:** TCP, default port `8080`
- **Byte order:** little-endian (x64 native)
- **Element type:** IEEE-754 `float32` only
- **Connection model:** persistent — a client may send any number of request/response cycles over one socket; the server never closes a healthy connection

## Request

```
┌──────────────────────┬────────────────────────────────────┐
│ element_count        │ payload                            │
│ uint64 LE (8 bytes)  │ element_count × float32 LE (4 B ea)│
└──────────────────────┴────────────────────────────────────┘
```

`element_count` must equal the total element count of the model's input tensor — the product of all input-shape dimensions after dynamic dims are pinned to 1 (for the bundled dummy model with shape `[1, 10]`: **10**).

## Response

Identical layout:

```
┌──────────────────────┬────────────────────────────────────┐
│ element_count        │ payload                            │
│ uint64 LE (8 bytes)  │ element_count × float32 LE (4 B ea)│
└──────────────────────┴────────────────────────────────────┘
```

`element_count` is the product of the model's output-shape dimensions. Exactly one response is sent per complete request, in request order.

### Error signaling

There are no status codes. If inference fails — including an `element_count` that doesn't match the model's input size — the server responds with `element_count = 0` and an empty payload. Clients should treat a zero-length response as an error.

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
