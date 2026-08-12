#!/usr/bin/env python3
"""Keep public symbols in their originally assigned WRVISA version nodes."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "cmake/webreal_visa_abi.json"
MAP = ROOT / "cmake/webreal_visa.map"
HEADER = ROOT / "include/webreal_visa_ext.h"
VISA_HEADER = ROOT / "include/visa.h"
CMAKE = ROOT / "CMakeLists.txt"
NODE_PATTERN = re.compile(
    r"(WRVISA_\d+\.\d+)\s*\{\s*global:(.*?)\}\s*(?:WRVISA_\d+\.\d+)?\s*;",
    re.DOTALL,
)
SYMBOL_PATTERN = re.compile(r"\b((?:vi|wrvisa)[A-Za-z0-9_]+)\s*;")
DECLARATION_PATTERN = re.compile(
    r"WRVISA_API\s+ViStatus\s+WRVISA_CALL\s+((?:vi|wrvisa)[A-Za-z0-9_]+)\s*\(",
    re.DOTALL,
)


def map_history() -> dict[str, list[str]]:
    text = MAP.read_text(encoding="utf-8")
    return {
        node: sorted(SYMBOL_PATTERN.findall(body))
        for node, body in NODE_PATTERN.findall(text)
    }


def main() -> int:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    expected = {
        node: sorted(symbols)
        for node, symbols in manifest["symbol_versions"].items()
    }
    actual = map_history()
    errors: list[str] = []
    if actual != expected:
        for node in sorted(set(actual) | set(expected)):
            if actual.get(node) != expected.get(node):
                errors.append(
                    f"{node}: expected {expected.get(node, [])}, got {actual.get(node, [])}"
                )

    expected_symbols = {
        symbol for symbols in expected.values() for symbol in symbols
    }
    declared_symbols = set(
        DECLARATION_PATTERN.findall(
            VISA_HEADER.read_text(encoding="utf-8")
            + "\n"
            + HEADER.read_text(encoding="utf-8")
        )
    )
    if declared_symbols != expected_symbols:
        missing = sorted(expected_symbols - declared_symbols)
        extra = sorted(declared_symbols - expected_symbols)
        if missing:
            errors.append("public headers omit: " + ", ".join(missing))
        if extra:
            errors.append("ABI manifest omits declarations: " + ", ".join(extra))

    version = manifest["project_version"]
    major, minor, patch = version.split(".")
    header = HEADER.read_text(encoding="utf-8")
    required_header = (
        f"#define WRVISA_VERSION_MAJOR {major}",
        f"#define WRVISA_VERSION_MINOR {minor}",
        f"#define WRVISA_VERSION_PATCH {patch}",
        f'#define WRVISA_VERSION_STRING "{version}"',
    )
    for declaration in required_header:
        if declaration not in header:
            errors.append(f"extension header version mismatch: {declaration}")
    if f"project(webreal_visa VERSION {version} " not in CMAKE.read_text(
        encoding="utf-8"
    ):
        errors.append(f"CMake project version is not {version}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    symbol_count = len(expected_symbols)
    print(
        f"ABI history valid; {symbol_count} symbols across {len(actual)} version nodes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
