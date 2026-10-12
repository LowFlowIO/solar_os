from pathlib import Path
import sys
assets, output = map(Path, sys.argv[1:])
lines = ['#include <stddef.h>', '#include <stdint.h>']
for filename, symbol in [('bios.bin', 'tiny_bios'), ('vgabios.bin', 'tiny_vgabios')]:
    data = (assets / filename).read_bytes()
    lines.append('const uint8_t ' + symbol + '[] = {')
    for offset in range(0, len(data), 16):
        lines.append(','.join(str(byte) for byte in data[offset:offset + 16]) + ',')
    lines += ['};', 'const size_t ' + symbol + '_size = sizeof(' + symbol + ');']
output.write_text('\n'.join(lines) + '\n')
