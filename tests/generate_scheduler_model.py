"""Generate a deterministic dense CPU benchmark model with exact y = 2x outputs."""

import argparse

import onnx
from onnx import TensorProto, helper


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output")
    parser.add_argument("--width", type=int, default=256, choices=(128, 256, 512))
    args = parser.parse_args()
    width = args.width

    # Expansion duplicates the input row. All dense weight rows sum to one,
    # so each MatMul preserves the duplicated rows. The benchmark inputs and
    # every intermediate sum are small integers exactly representable as float32.
    weights = [
        (1 if (column - row) % width < width // 2 else -1) + (column == row)
        for row in range(width)
        for column in range(width)
    ]
    constants = [
        helper.make_tensor("shape", TensorProto.INT64, [2], [width, 10]),
        helper.make_tensor("weights", TensorProto.FLOAT, [width, width], weights),
        helper.make_tensor("row_zero", TensorProto.INT64, [1], [0]),
        helper.make_tensor("two", TensorProto.FLOAT, [1], [2]),
    ]
    nodes = [helper.make_node("Expand", ["x", "shape"], ["wide0"])]
    for i in range(1, 7):
        nodes.append(helper.make_node("MatMul", ["weights", f"wide{i - 1}"], [f"wide{i}"]))
    nodes.extend([
        helper.make_node("Gather", ["wide6", "row_zero"], ["first"], axis=0),
        helper.make_node("Mul", ["first", "two"], ["y"]),
    ])
    graph = helper.make_graph(
        nodes, "scheduler_dense", [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 10])],
        [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 10])], constants,
    )
    model = helper.make_model(graph, ir_version=8, opset_imports=[helper.make_opsetid("", 13)])
    onnx.checker.check_model(model)
    onnx.save(model, args.output)


if __name__ == "__main__":
    main()
