from __future__ import annotations

from pathlib import Path

LUT_SIZE = 1025
Q16 = 1 << 16
STEP = 64


def recip_entry(index: int) -> int:
    if index < 0 or index >= LUT_SIZE:
        raise ValueError(index)

    sample = Q16 + index * STEP

    return round((1 << 32) / sample)


def write_hex(path: Path) -> None:
    with path.open("w", encoding="ascii", newline="\n") as handle:
        for index in range(LUT_SIZE):
            handle.write(f"{recip_entry(index):08x}\n")


def main() -> int:
    assets = Path(__file__).resolve().parents[1] / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    out_path = assets / "recip_lut.hex"
    write_hex(out_path)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
