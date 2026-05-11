PYTHON ?= python3
PYTHON_ENV = PYTHONDONTWRITEBYTECODE=1

.PHONY: diagrams breakdown

diagrams:
	mkdir -p build/diagrams docs/diagrams
	./scripts/gen_netlist_json.sh || echo "WARNING: Yosys netlist JSON not produced; see build/diagrams/yosys_voxel_gpu.log"
	$(PYTHON_ENV) $(PYTHON) scripts/gen_datapath_diagram.py
	$(PYTHON_ENV) $(PYTHON) scripts/gen_pipeline_diagram.py
	./scripts/run_voxel_gpu_tb.sh || echo "WARNING: voxel_gpu testbench did not run cleanly; see build/diagrams/voxel_gpu_tb.log"
	$(PYTHON_ENV) $(PYTHON) scripts/gen_timing_diagram.py
	$(PYTHON_ENV) $(PYTHON) scripts/render_timing_diagram.py
	$(PYTHON_ENV) $(PYTHON) scripts/gen_diagram_viewer.py

breakdown:
	mkdir -p build/diagrams docs/diagrams
	$(PYTHON_ENV) $(PYTHON) scripts/gen_breakdown_docs.py
	$(PYTHON_ENV) $(PYTHON) scripts/gen_diagram_viewer.py
