#!/usr/bin/env python3
"""Generate source-grounded voxel_gpu pipeline/control diagrams."""

from diagram_source_model import generate_common_diagrams, write_source_model_json


PIPELINE_OUTPUTS = [
    "voxel_gpu_pipeline.mmd",
    "voxel_gpu_control_fsm.mmd",
]


def main() -> int:
    outputs = generate_common_diagrams(PIPELINE_OUTPUTS)
    outputs.append(write_source_model_json())
    for path in outputs:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

