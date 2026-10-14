#!/usr/bin/env python3
"""Keep ELF virtual text bounds independent of allocation alignment."""
import argparse
from pathlib import Path

OLD = "elf->sec[ELF_SEC_TEXT].size    = ELF_ALIGN(shdr[i].size, 4);"
NEW = "elf->sec[ELF_SEC_TEXT].size    = shdr[i].size; /* Virtual bounds exclude padding. */"


def patch(component: Path):
    source = component / "src/esp_elf.c"
    text = source.read_text()
    if NEW in text:
        return
    if text.count(OLD) != 1:
        raise ValueError("ELF loader source changed; review the text-bound overlay")
    source.write_text(text.replace(OLD, NEW))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("component", type=Path)
    patch(parser.parse_args().component)
