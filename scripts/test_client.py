"""Single-request smoke test for the running inference server.

Connects to 127.0.0.1:8080, sends 10 floats using the binary protocol
(uint64 LE element count + float32 LE payload — see docs/PROTOCOL.md), and
prints the response. With the dummy model from generate_dummy.py, every
output value must be exactly double its input.

Usage (server must be running):
    uv run scripts/test_client.py
"""

import socket
import struct

def test_inference():
    """Send one inference request and print the input/output pair."""
    # Настройки
    HOST = '127.0.0.1'
    PORT = 8080
    
    # Готовим данные (10 float-значений, как ожидает наша dummy-модель)
    input_data = [1.0, 2.5, 3.14, 4.0, 5.5, 6.0, 7.7, 8.0, 9.9, 10.0]
    element_count = len(input_data)
    
    print(f"[INFO] Connecting to {HOST}:{PORT}...")
    
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))
        
        # Упаковка данных:
        # '<Q' - little-endian unsigned long long (8 байт для size_t на x64)
        # f'<{element_count}f' - little-endian floats (4 байта на каждый float)
        header = struct.pack('<Q', element_count)
        payload = struct.pack(f'<{element_count}f', *input_data)
        
        print(f"[INFO] Sending {element_count} elements...")
        s.sendall(header + payload)
        
        # Чтение заголовка ответа (size_t)
        response_header = s.recv(8)
        if not response_header or len(response_header) < 8:
            print("[ERROR] Failed to receive header.")
            return
            
        out_element_count = struct.unpack('<Q', response_header)[0]
        
        # Чтение полезной нагрузки (массив floats)
        expected_bytes = out_element_count * 4
        response_payload = b''
        
        while len(response_payload) < expected_bytes:
            packet = s.recv(expected_bytes - len(response_payload))
            if not packet:
                break
            response_payload += packet
            
        if len(response_payload) == expected_bytes:
            output_data = struct.unpack(f'<{out_element_count}f', response_payload)
            print(f"[SUCCESS] Received {out_element_count} elements.")
            print("[INFO] Input :", [round(x, 2) for x in input_data])
            print("[INFO] Output:", [round(x, 2) for x in output_data])
        else:
            print("[ERROR] Incomplete payload received.")

if __name__ == "__main__":
    test_inference()