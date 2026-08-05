#!/usr/bin/env python3
"""Run liminal-dev-mcp without locking its canonical Windows build output."""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
TARGET_ENV = "LIMINAL_DEV_MCP_BINARY"
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


def is_executable_binary(path: Path) -> bool:
    expected_magic = b"MZ" if os.name == "nt" else b"\x7fELF"
    try:
        with path.open("rb") as executable:
            return executable.read(len(expected_magic)) == expected_magic
    except OSError:
        return False


def configured_target() -> Path:
    if override := os.environ.get(TARGET_ENV):
        target = Path(override)
    else:
        result = subprocess.run(
            ["xmake", "show", "-t", "liminal-dev-mcp"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr.strip() or "cannot inspect xmake target")
        output = ANSI_ESCAPE.sub("", result.stdout)
        match = re.search(r"^\s*targetfile:\s*(.+?)\s*$", output, re.MULTILINE)
        if not match:
            raise RuntimeError("xmake did not report the liminal-dev-mcp target file")
        target = Path(match.group(1))
        if not target.is_absolute():
            target = REPO_ROOT / target

    if not is_executable_binary(target):
        raise RuntimeError(f"liminal-dev-mcp is missing or invalid: {target}")
    return target


def shadow_copy(target: Path) -> Path:
    runtime_directory = target.parent / ".liminal-dev-mcp-runtime"
    runtime_directory.mkdir(exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=runtime_directory, prefix=".copy-", suffix=".exe"
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        shutil.copy2(target, temporary)
        if not is_executable_binary(temporary):
            raise RuntimeError(f"copied liminal-dev-mcp is invalid: {temporary}")
        with temporary.open("rb") as executable:
            digest = hashlib.file_digest(executable, "sha256").hexdigest()
        snapshot = runtime_directory / f"liminal-dev-mcp-{digest}.exe"
        if snapshot.exists():
            temporary.unlink()
        else:
            try:
                temporary.rename(snapshot)
            except FileExistsError:
                temporary.unlink()

        for stale in runtime_directory.iterdir():
            if stale == snapshot:
                continue
            try:
                stale.unlink()
            except PermissionError:
                # Another Codex session can still be running this snapshot.
                pass
        return snapshot
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    try:
        target = configured_target()
        executable = shadow_copy(target) if os.name == "nt" else target
        return subprocess.call([str(executable)], cwd=REPO_ROOT)
    except (OSError, RuntimeError) as error:
        print(f"liminal-dev-mcp launcher: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
