"""Export a trivial PyTorch model to models/model.onnx for end-to-end testing.

The model computes y = 2x over a fixed [1, 10] float32 tensor, so a correct
server round-trip is trivially verifiable: every returned value must be twice
the value sent (see scripts/test_client.py).

Usage:
    uv run scripts/generate_dummy.py
"""

import torch
import os

class DummyModel(torch.nn.Module):
    def forward(self, x):
        # Simple multiplication to easily verify the TCP response
        return x * 2.0

model = DummyModel()
model.eval()

# Explicitly defining the shape (e.g., 10 floats)
dummy_input = torch.randn(1, 10) 

# Resolve paths robustly regardless of where the script is executed from
current_dir = os.path.dirname(os.path.abspath(__file__))
model_path = os.path.join(current_dir, "..", "models", "model.onnx")

torch.onnx.export(
    model, 
    dummy_input, 
    model_path, 
    input_names=['input'], 
    output_names=['output'],
    opset_version=17
)

print(f"[INFO] Dummy model saved to {model_path}. Expected input: 10 floats.")