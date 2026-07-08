"""Render metrics.jsonl into a two-panel dashboard (metrics_dashboard.png).

Top panel: inference latency over time (server-side RunInference wall clock).
Bottom panel: input mean over time — a step change indicates input data
drift, e.g. the deliberate 5x shift injected by scripts/load_test.py.

Usage (after running the server under load):
    uv run scripts/analyze_metrics.py
"""

import json
import os
import matplotlib.pyplot as plt

def main():
    current_dir = os.path.dirname(os.path.abspath(__file__))
    log_file = os.path.join(current_dir, "..", "metrics.jsonl")
    dashboard_file = os.path.join(current_dir, "..", "metrics_dashboard.png")

    timestamps = []
    latencies = []
    means = []

    try:
        with open(log_file, 'r') as f:
            for line in f:
                data = json.loads(line)
                timestamps.append(data['timestamp'])
                latencies.append(data['latency_ms'])
                means.append(data['input_mean'])
    except FileNotFoundError:
        print(f"[ERROR] {log_file} not found. Run load_test.py first.")
        return

    if not timestamps:
        print("[WARN] No metrics data to plot.")
        return

    # Normalize timestamps to relative seconds
    t0 = timestamps[0]
    time_sec = [(t - t0) / 1000.0 for t in timestamps]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8), sharex=True)

    # Latency Degradation Plot
    ax1.plot(time_sec, latencies, marker='o', markersize=3, linestyle='', alpha=0.6, color='#d62728')
    ax1.set_ylabel('Latency (ms)')
    ax1.set_title('Inference Latency under Concurrent Load')
    ax1.grid(True, linestyle='--', alpha=0.7)

    # Data Drift Detection Plot
    ax2.plot(time_sec, means, marker='x', markersize=4, linestyle='', alpha=0.6, color='#1f77b4')
    ax2.set_ylabel('Input Mean')
    ax2.set_xlabel('Time (seconds)')
    ax2.set_title('Input Data Drift Detection')
    ax2.grid(True, linestyle='--', alpha=0.7)

    plt.tight_layout()
    plt.savefig(dashboard_file)
    print(f"[SUCCESS] Automated dashboard generated: {dashboard_file}")

if __name__ == "__main__":
    main()