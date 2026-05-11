#!/usr/bin/env python3
"""Generate source-grounded architecture/datapath Mermaid diagrams."""

from diagram_source_model import (
    generate_common_diagrams,
    write_source_model_json,
)


DATAPATH_OUTPUTS = [
    "full_system_architecture.mmd",
    "hps_fpga_ownership.mmd",
    "hps_software_architecture.mmd",
    "hps_to_fpga_dataflow.mmd",
    "register_interface_flow.mmd",
    "soc_system_context.mmd",
    "voxel_gpu_module_hierarchy.mmd",
    "voxel_gpu_datapath.mmd",
    "memory_and_buffer_ownership.mmd",
    "game_to_pixels_flow.mmd",
    "c_call_graph.mmd",
]


def main() -> int:
    outputs = generate_common_diagrams(DATAPATH_OUTPUTS)
    outputs.append(write_source_model_json())
    for path in outputs:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

