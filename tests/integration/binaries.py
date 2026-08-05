"""Locate validated executables from the active xmake configuration."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path


ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


def _is_executable_binary(path: Path) -> bool:
    expected_magic = b"MZ" if os.name == "nt" else b"\x7fELF"
    try:
        with path.open("rb") as executable:
            return executable.read(len(expected_magic)) == expected_magic
    except OSError:
        return False


def _configured_target(repo_root: Path, target: str) -> Path | None:
    try:
        result = subprocess.run(
            ["xmake", "show", "-t", target],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None

    output = ANSI_ESCAPE.sub("", result.stdout)
    match = re.search(r"^\s*targetfile:\s*(.+?)\s*$", output, re.MULTILINE)
    if not match:
        return None
    path = Path(match.group(1))
    return path if path.is_absolute() else repo_root / path


def find_binary(repo_root: Path, target: str, override_variable: str) -> Path | None:
    if override := os.environ.get(override_variable):
        candidate = Path(override)
        if not _is_executable_binary(candidate):
            raise RuntimeError(
                f"{override_variable} does not reference a valid executable: {candidate}"
            )
        return candidate

    configured = _configured_target(repo_root, target)
    if configured is not None:
        if not _is_executable_binary(configured):
            raise RuntimeError(
                f"xmake target '{target}' is missing or invalid: {configured}"
            )
        return configured

    executable = f"{target}.exe" if os.name == "nt" else target
    candidates = [
        candidate
        for build_dir in repo_root.glob("build*")
        for mode in ("releasedbg", "release")
        for candidate in build_dir.glob(f"*/*/{mode}/{executable}")
        if _is_executable_binary(candidate)
    ]
    return max(
        candidates, key=lambda candidate: candidate.stat().st_mtime, default=None
    )
