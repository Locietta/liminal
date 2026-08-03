#!/usr/bin/env python3
"""Format project files with the formatter selected from their extension."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

FORMATTERS = {
    "clang-format": {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"},
    "ruff": {".py", ".pyi"},
    "prettier": {".json", ".md"},
    "taplo": {".toml"},
}

COMMANDS = {
    "clang-format": ["clang-format", "-i"],
    "ruff": ["ruff", "format"],
    "prettier": ["prettier", "--write"],
    "taplo": ["taplo", "format"],
}

LANGUAGES = {
    "cpp": "clang-format",
    "json": "prettier",
    "markdown": "prettier",
    "python": "ruff",
    "toml": "taplo",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Format tracked project files, selecting a formatter by extension."
    )
    parser.add_argument(
        "paths",
        nargs="+",
        type=Path,
        help="files or directories to format",
    )
    parser.add_argument(
        "-l",
        "--language",
        choices=LANGUAGES,
        help="override language detection for every input",
    )
    return parser.parse_args()


def project_files(paths: list[Path]) -> list[Path]:
    for path in paths:
        if not path.exists():
            raise FileNotFoundError(path)
    command = [
        "git",
        "ls-files",
        "-z",
        "--cached",
        "--others",
        "--exclude-standard",
        "--",
        *map(str, paths),
    ]

    output = subprocess.check_output(command)
    files = [Path(path.decode()) for path in output.split(b"\0") if path]
    return [path for path in files if path.is_file()]


def select_formatters(paths: list[Path], language: str | None) -> dict[str, list[Path]]:
    if language:
        return {LANGUAGES[language]: paths}

    selected: dict[str, list[Path]] = defaultdict(list)
    for path in paths:
        suffix = path.suffix.lower()
        if not suffix and path.is_file():
            with path.open("rb") as file:
                if b"python" in file.readline(128).lower():
                    selected["ruff"].append(path)
                    continue
        for formatter, extensions in FORMATTERS.items():
            if suffix in extensions:
                selected[formatter].append(path)
                break
    return selected


def language_arguments(language: str | None, files: list[Path]) -> list[str]:
    if language in {"json", "markdown"}:
        return ["--parser", language]
    if language == "python":
        extensions = sorted(
            {
                path.suffix[1:].lower()
                for path in files
                if path.suffix and path.suffix.lower() not in {".py", ".pyi"}
            }
        )
        return [
            argument
            for extension in extensions
            for argument in ("--extension", f"{extension}:python")
        ]
    return []


def run_formatter(formatter: str, files: list[Path], language: str | None) -> int:
    executable = shutil.which(COMMANDS[formatter][0])
    if executable is None:
        print(f"format: could not find {formatter}", file=sys.stderr)
        return 1

    if formatter == "taplo" and language == "toml":
        for file in files:
            result = subprocess.run(
                [
                    executable,
                    *COMMANDS[formatter][1:],
                    "--stdin-filepath",
                    f"{file}.toml",
                    "-",
                ],
                input=file.read_bytes(),
                stdout=subprocess.PIPE,
                check=False,
            )
            if result.returncode != 0:
                return result.returncode
            file.write_bytes(result.stdout)
        return 0

    command = [
        executable,
        *COMMANDS[formatter][1:],
        *language_arguments(language, files),
        *map(str, files),
    ]
    if Path(executable).suffix.lower() in {".bat", ".cmd"}:
        return subprocess.run(
            subprocess.list2cmdline(command), shell=True, check=False
        ).returncode
    return subprocess.run(command, check=False).returncode


def main() -> int:
    args = parse_args()
    try:
        paths = project_files(args.paths)
    except (FileNotFoundError, subprocess.CalledProcessError) as error:
        print(f"format: {error}", file=sys.stderr)
        return 1

    if not paths:
        print("No project files found in the provided paths.")
        return 0

    selected = select_formatters(paths, args.language)
    if not selected:
        print("No supported files to format.")
        return 0

    for formatter, files in selected.items():
        print(f"Formatting {len(files)} file(s) with {formatter}...", flush=True)
        returncode = run_formatter(formatter, files, args.language)
        if returncode != 0:
            return returncode

    return 0


if __name__ == "__main__":
    sys.exit(main())
