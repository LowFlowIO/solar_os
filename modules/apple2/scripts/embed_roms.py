"""Embed the module's Apple II Plus and Disk II firmware images."""

from pathlib import Path
import sys

assets, output = map(Path, sys.argv[1:])
lines = ['#include "roms.h"']
for filename, symbol, size in [
    ("appleII+.rom", "solar_os_apple2_rom", 12288),
    ("diskII.rom", "solar_os_apple2_disk_rom", 256),
]:
    data = (assets / filename).read_bytes()
    if len(data) != size:
        raise SystemExit(f"{filename}: expected {size} bytes, got {len(data)}")
    lines.append(f"const uint8_t {symbol}[{size}] = {{")
    for offset in range(0, size, 16):
        lines.append(",".join(str(byte) for byte in data[offset:offset + 16]) + ",")
    lines.append("};")
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text("\n".join(lines) + "\n")
