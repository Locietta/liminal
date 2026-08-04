#!/usr/bin/env python3
"""Run the current Windows worktree in a disposable WSL directory."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build and test a disposable snapshot of this worktree in WSL."
    )
    parser.add_argument("--distro", default="Arch", help="WSL distribution name")
    parser.add_argument(
        "--mode",
        choices=("normal", "release", "sanitize"),
        default="normal",
        help="configuration to validate",
    )
    args = parser.parse_args()

    if shutil.which("wsl.exe") is None:
        print("test-wsl: wsl.exe was not found", file=sys.stderr)
        return 1

    repo = Path(
        subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"], text=True
        ).strip()
    )
    return subprocess.run(
        [
            "wsl.exe",
            "-d",
            args.distro,
            "--cd",
            str(repo),
            "--",
            "bash",
            "scripts/test-wsl.sh",
            ".",
            args.mode,
        ],
        check=False,
    ).returncode


if __name__ == "__main__":
    sys.exit(main())
