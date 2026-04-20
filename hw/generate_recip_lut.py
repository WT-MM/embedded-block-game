from __future__ import annotations

from pathlib import Path

LUT_SIZE = 1024
Q16 = 1 << 16


def recip_entry(index: int) -> int:
    if index < 0 or index >= LUT_SIZE:
        raise ValueError(index)

    if index == 0:
        sample = Q16
    else:
        sample = Q16 + index * 64 + 32

    return round((1 << 32) / sample)


def write_hex(path: Path) -> None:
    with path.open("w", encoding="ascii", newline="\n") as handle:
        for index in range(LUT_SIZE):
            handle.write(f"{recip_entry(index):08x}\n")


def main() -> int:
    out_path = Path(__file__).with_name("recip_lut.hex")
    write_hex(out_path)
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
