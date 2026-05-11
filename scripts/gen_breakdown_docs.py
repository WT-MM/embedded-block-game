#!/usr/bin/env python3
"""Generate source-grounded Markdown breakdown documents."""

from diagram_source_model import generate_breakdown_docs, write_source_model_json


def main() -> int:
    outputs = generate_breakdown_docs()
    outputs.append(write_source_model_json())
    for path in outputs:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

