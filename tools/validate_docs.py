#!/usr/bin/env python3
"""Validate the repository's AI-maintenance documentation contract."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REQUIRED = (
    "AGENTS.md",
    "docs/status/current.md",
    "docs/project/requirements.md",
    "docs/project/licensing.md",
    "docs/architecture/overview.md",
    "docs/architecture/code-map.md",
    "docs/compatibility/visa-compatibility.md",
    "docs/research/sources.md",
)
LINK_PATTERN = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
ADR_FILE_PATTERN = re.compile(r"^(\d{4})-[a-z0-9-]+\.md$")


def tracked_files() -> list[str]:
    result: list[str] = []
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(ROOT).as_posix()
        if relative == "CMakeLists.txt":
            result.append(relative)
        elif relative.startswith(("include/", "src/", "tests/", "tools/", "cmake/", "examples/")):
            if path.suffix in {".h", ".c", ".cc", ".cpp", ".py", ".cmake", ".json"}:
                result.append(relative)
            elif path.name == "CMakeLists.txt" or path.suffix == ".in":
                result.append(relative)
        elif relative.startswith(".github/workflows/") and path.suffix in {
            ".yml",
            ".yaml",
        }:
            result.append(relative)
    return sorted(result)


def check_links(errors: list[str]) -> None:
    for document in ROOT.rglob("*.md"):
        text = document.read_text(encoding="utf-8")
        for raw_target in LINK_PATTERN.findall(text):
            target = raw_target.strip().strip("<>")
            if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                continue
            file_part = target.split("#", 1)[0]
            resolved = (document.parent / file_part).resolve()
            try:
                resolved.relative_to(ROOT)
            except ValueError:
                errors.append(f"{document.relative_to(ROOT)}: link escapes repository: {target}")
                continue
            if not resolved.exists():
                errors.append(f"{document.relative_to(ROOT)}: broken link: {target}")


def check_adrs(errors: list[str]) -> None:
    seen: set[str] = set()
    decisions = ROOT / "docs/decisions"
    for path in sorted(decisions.glob("*.md")):
        if path.name == "README.md":
            continue
        match = ADR_FILE_PATTERN.match(path.name)
        if not match:
            errors.append(f"invalid ADR filename: {path.relative_to(ROOT)}")
            continue
        identifier = match.group(1)
        if identifier in seen:
            errors.append(f"duplicate ADR identifier: {identifier}")
        seen.add(identifier)
        first_line = path.read_text(encoding="utf-8").splitlines()[0]
        if f"ADR-{identifier}" not in first_line:
            errors.append(f"{path.relative_to(ROOT)}: heading does not match filename")


def main() -> int:
    errors: list[str] = []
    for relative in REQUIRED:
        if not (ROOT / relative).is_file():
            errors.append(f"missing required document: {relative}")

    check_links(errors)
    check_adrs(errors)

    code_map_path = ROOT / "docs/architecture/code-map.md"
    if code_map_path.is_file():
        code_map = code_map_path.read_text(encoding="utf-8")
        for relative in tracked_files():
            if f"`{relative}`" not in code_map:
                errors.append(f"code map does not cover: {relative}")

    status_path = ROOT / "docs/status/current.md"
    if status_path.is_file():
        status = status_path.read_text(encoding="utf-8")
        required_status_terms = (
            "0.4",
            "TCPIP",
            "SOCKET",
            "ASRL",
            "VXI-11",
            "HiSLIP",
            "NOT_TESTED",
        )
        missing_terms = [term for term in required_status_terms if term not in status]
        if missing_terms:
            errors.append(
                "current status omits the 0.4 transport/platform boundary: "
                + ", ".join(missing_terms)
            )

        platform_contracts = {
            "docs/project/requirements.md": (
                "0.4 无硬件兼容性",
                "Windows 原生无硬件网络/协议验证",
                "Windows ASRL runtime",
            ),
            "docs/compatibility/visa-compatibility.md": (
                "资源 alias",
                "Implemented (Linux/Windows simulator)",
                "Windows ASRL runtime `NOT_TESTED`",
            ),
        }
        for relative, required_terms in platform_contracts.items():
            path = ROOT / relative
            if not path.is_file():
                continue
            text = path.read_text(encoding="utf-8")
            missing = [term for term in required_terms if term not in text]
            if missing:
                errors.append(
                    f"{relative}: Windows verification boundary is stale: "
                    + ", ".join(missing)
                )

    licensing_path = ROOT / "docs/project/licensing.md"
    if licensing_path.is_file():
        licensing = licensing_path.read_text(encoding="utf-8")
        if "[TBD_COPYRIGHT_HOLDER]" in licensing and (ROOT / "LICENSE").exists():
            errors.append("formal LICENSE exists while copyright holder remains TBD")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"documentation contract valid; mapped {len(tracked_files())} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
