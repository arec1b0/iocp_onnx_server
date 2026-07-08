"""Concurrent load test exercising TCP reassembly and drift detection.

Spawns 20 client threads, each sending 50 requests to 127.0.0.1:8080. Two
failure modes are provoked on purpose:

- **TCP fragmentation:** every request is sent in random-sized chunks with
  small delays, forcing the server's accumulation buffer / state machine to
  reassemble partial headers and payloads.
- **Input data drift:** halfway through each client's run the input mean
  jumps 5x, which must show up as a step change in the ``input_mean`` series
  of metrics.jsonl (visualize with scripts/analyze_metrics.py).

Usage (server must be running):
    uv run scripts/load_test.py
"""

import socket
import struct
import threading
import time
import random

def simulate_client(client_id, host, port, element_count, iterations):
    """Run one client: `iterations` fragmented request/response cycles."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((host, port))
            for i in range(iterations):
                # Simulate data drift: mean shifts significantly halfway through the test
                drift_factor = 1.0 if i < (iterations / 2) else 5.0
                input_data = [random.random() * drift_factor for _ in range(element_count)]
                
                header = struct.pack('<Q', element_count)
                payload = struct.pack(f'<{element_count}f', *input_data)
                
                full_message = header + payload
                
                # Intentional TCP fragmentation
                chunk_size = random.randint(1, max(1, len(full_message) // 3))
                for j in range(0, len(full_message), chunk_size):
                    s.sendall(full_message[j:j+chunk_size])
                    time.sleep(0.01) # Force network delay and buffer accumulation
                    
                response_header = s.recv(8)
                if not response_header or len(response_header) < 8:
                    print(f"[ERROR] Client {client_id}: Header read failed.")
                    break
                    
                out_element_count = struct.unpack('<Q', response_header)[0]
                expected_bytes = out_element_count * 4
                
                response_payload = b''
                while len(response_payload) < expected_bytes:
                    packet = s.recv(expected_bytes - len(response_payload))
                    if not packet:
                        break
                    response_payload += packet
                    
    except Exception as e:
        print(f"[ERROR] Client {client_id} failed: {e}")

def main():
    host = '127.0.0.1'
    port = 8080
    threads = []
    
    num_clients = 20
    iterations_per_client = 50
    element_count = 10
    
    print(f"[INFO] Starting Load Test: {num_clients} concurrent clients, {iterations_per_client} requests each.")
    print("[INFO] Simulating TCP fragmentation and input data drift...")
    start_time = time.time()
    
    for i in range(num_clients):
        t = threading.Thread(target=simulate_client, args=(i, host, port, element_count, iterations_per_client))
        threads.append(t)
        t.start()
        
    for t in threads:
        t.join()
        
    end_time = time.time()
    print(f"[SUCCESS] Load Test Completed in {end_time - start_time:.2f} seconds.")
    print("[INFO] Check metrics.jsonl for inference latency and drift statistics.")

if __name__ == "__main__":
    main()