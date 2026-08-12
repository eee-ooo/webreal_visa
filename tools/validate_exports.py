#!/usr/bin/env python3
"""Validate ELF or PE named exports against the public ABI allowlist."""

from __future__ import annotations

import re
import struct
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAP_PATH = ROOT / "cmake/webreal_visa.map"
SYMBOL_PATTERN = re.compile(r"^\s*((?:vi|wrvisa)[A-Za-z0-9_]+);\s*$")


def expected_exports() -> set[str]:
    exports = {
        match.group(1)
        for line in MAP_PATH.read_text(encoding="utf-8").splitlines()
        if (match := SYMBOL_PATTERN.match(line))
    }
    if not exports:
        raise RuntimeError("public export allowlist is empty")
    return exports


def rva_to_offset(data: bytes, section_table: int, section_count: int, rva: int) -> int:
    for index in range(section_count):
        offset = section_table + index * 40
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8
        )
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_offset + rva - virtual_address
    raise RuntimeError(f"PE RVA 0x{rva:x} is outside every section")


def pe_exports(path: Path) -> set[str]:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise RuntimeError("not a PE image")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise RuntimeError("invalid PE signature")
    coff = pe_offset + 4
    section_count = struct.unpack_from("<H", data, coff + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    magic = struct.unpack_from("<H", data, optional)[0]
    if magic == 0x20B:
        data_directories = optional + 112
    elif magic == 0x10B:
        data_directories = optional + 96
    else:
        raise RuntimeError(f"unsupported PE optional-header magic 0x{magic:x}")
    export_rva = struct.unpack_from("<I", data, data_directories)[0]
    if export_rva == 0:
        return set()
    section_table = optional + optional_size
    export_offset = rva_to_offset(data, section_table, section_count, export_rva)
    fields = struct.unpack_from("<IIHHIIIIIII", data, export_offset)
    name_count = fields[7]
    names_rva = fields[9]
    names_offset = rva_to_offset(data, section_table, section_count, names_rva)
    exports: set[str] = set()
    for index in range(name_count):
        name_rva = struct.unpack_from("<I", data, names_offset + index * 4)[0]
        name_offset = rva_to_offset(data, section_table, section_count, name_rva)
        end = data.index(b"\0", name_offset)
        exports.add(data[name_offset:end].decode("ascii"))
    return exports


def elf_exports(path: Path) -> set[str]:
    completed = subprocess.run(
        ["readelf", "--dyn-syms", "--wide", str(path)],
        check=True,
        capture_output=True,
        text=True,
    )
    exports: set[str] = set()
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[4] not in {"GLOBAL", "WEAK"} or fields[6] == "UND":
            continue
        name = fields[7].split("@", 1)[0]
        if not name.startswith("WRVISA_"):
            exports.add(name)
    return exports


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <shared-library>", file=sys.stderr)
        return 2
    library = Path(sys.argv[1])
    if not library.is_file():
        print(f"ERROR: shared library does not exist: {library}", file=sys.stderr)
        return 1
    try:
        actual = pe_exports(library) if library.suffix.lower() == ".dll" else elf_exports(library)
        expected = expected_exports()
    except (OSError, RuntimeError, struct.error, subprocess.CalledProcessError) as error:
        print(f"ERROR: cannot inspect {library}: {error}", file=sys.stderr)
        return 1
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing or extra:
        if missing:
            print("ERROR: missing exports: " + ", ".join(missing), file=sys.stderr)
        if extra:
            print("ERROR: unexpected exports: " + ", ".join(extra), file=sys.stderr)
        return 1
    print(f"export contract valid; {len(actual)} named exports in {library.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
