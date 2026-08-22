#!/usr/bin/env python3
"""Build, package, and locally install a portable Liminal release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections import deque
from pathlib import Path
from typing import Any

import tomllib

PROJECT_ROOT = Path(__file__).resolve().parents[1]
PACKAGE_ROOT = PROJECT_ROOT / "build" / "package"
ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
DLL_IMPORT = re.compile(r"DLL Name:\s*(\S+)", re.IGNORECASE)


def run(command: list[str], *, environment: dict[str, str]) -> None:
    print(f"+ {subprocess.list2cmdline(command)}", flush=True)
    subprocess.run(
        command,
        cwd=PROJECT_ROOT,
        env=environment,
        check=True,
    )


def project_version() -> str:
    with (PROJECT_ROOT / "pixi.toml").open("rb") as file:
        return tomllib.load(file)["workspace"]["version"]


def release_environment() -> dict[str, str]:
    environment = os.environ.copy()
    environment["XMAKE_CONFIGDIR"] = str(PACKAGE_ROOT / "config")
    return environment


def build_release() -> Path:
    environment = release_environment()
    build_dir = PACKAGE_ROOT / "xmake"
    configure = [
        "xmake",
        "f",
        "-m",
        "release",
        "--sanitizers=n",
        "--runtimes=stdc++_static",
        "-o",
        str(build_dir),
        "-y",
    ]
    run(configure, environment=environment)
    run(["xmake", "build", "-y", "liminal"], environment=environment)

    output = subprocess.check_output(
        ["xmake", "show", "-t", "liminal"],
        cwd=PROJECT_ROOT,
        env=environment,
        text=True,
        errors="replace",
    )
    output = ANSI_ESCAPE.sub("", output)
    match = re.search(r"^\s*targetfile:\s*(.+?)\s*$", output, re.MULTILINE)
    if match is None:
        raise RuntimeError("xmake did not report the liminal target path")

    target = Path(match.group(1))
    if not target.is_absolute():
        target = PROJECT_ROOT / target
    if not target.is_file():
        raise FileNotFoundError(f"release binary was not produced: {target}")
    return target.resolve()


def windows_runtime_directories(target: Path) -> list[Path]:
    directories = [target.parent]
    conda_prefix = os.environ.get("CONDA_PREFIX")
    if conda_prefix is None:
        raise RuntimeError("CONDA_PREFIX is unset; run this script through Pixi")

    prefix = Path(conda_prefix)
    directories.extend((prefix / "Library" / "bin", prefix / "bin"))
    return [directory for directory in directories if directory.is_dir()]


def runtime_file_index(directories: list[Path]) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for directory in directories:
        for path in directory.iterdir():
            if path.is_file():
                files.setdefault(path.name.casefold(), path)
    return files


def pe_imports(path: Path, objdump: str) -> list[str]:
    output = subprocess.check_output(
        [objdump, "-p", str(path)],
        text=True,
        errors="replace",
    )
    return DLL_IMPORT.findall(output)


def windows_runtime_files(target: Path) -> list[Path]:
    objdump = shutil.which("x86_64-w64-mingw32-objdump") or shutil.which("objdump")
    if objdump is None:
        raise RuntimeError(
            "could not find a PE-capable objdump in the Pixi environment"
        )

    files = runtime_file_index(windows_runtime_directories(target))
    system_root = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    system_files = runtime_file_index([system_root / "System32"])
    queued = deque([target])
    visited: set[str] = set()
    runtime_files: dict[str, Path] = {}
    unresolved: set[str] = set()
    uses_ucrt = False

    while queued:
        current = queued.popleft()
        current_key = current.name.casefold()
        if current_key in visited:
            continue
        visited.add(current_key)

        for imported_name in pe_imports(current, objdump):
            imported_key = imported_name.casefold()
            uses_ucrt = uses_ucrt or imported_key.startswith("api-ms-win-crt-")
            imported = files.get(imported_key)
            if imported is None:
                if imported_key not in system_files and not imported_key.startswith(
                    ("api-ms-win-", "ext-ms-win-")
                ):
                    unresolved.add(imported_name)
                continue
            if imported_key in visited:
                continue
            runtime_files[imported_key] = imported
            queued.append(imported)

    if uses_ucrt and (ucrtbase := files.get("ucrtbase.dll")) is not None:
        runtime_files["ucrtbase.dll"] = ucrtbase
    if unresolved:
        missing = ", ".join(sorted(unresolved, key=str.casefold))
        raise RuntimeError(f"could not resolve non-system runtime DLLs: {missing}")

    return sorted(runtime_files.values(), key=lambda path: path.name.casefold())


def conda_package_records() -> list[dict[str, Any]]:
    conda_prefix = os.environ.get("CONDA_PREFIX")
    if conda_prefix is None:
        raise RuntimeError("CONDA_PREFIX is unset; run this script through Pixi")

    records = []
    for path in sorted((Path(conda_prefix) / "conda-meta").glob("*.json")):
        with path.open(encoding="utf-8") as file:
            records.append(json.load(file))
    return records


def runtime_license_entries(runtime_files: list[Path]) -> list[tuple[str, Path]]:
    prefix = Path(os.environ["CONDA_PREFIX"]).resolve()
    records = conda_package_records()
    owners: dict[str, dict[str, Any]] = {}
    for record in records:
        for installed_file in record.get("files", []):
            owners.setdefault(str(installed_file).replace("\\", "/").casefold(), record)

    packages: dict[str, tuple[dict[str, Any], list[str]]] = {}
    for runtime_file in runtime_files:
        relative = runtime_file.resolve().relative_to(prefix).as_posix()
        record = owners.get(relative.casefold())
        if record is None:
            raise RuntimeError(f"could not identify the package owning {runtime_file}")
        name = str(record["name"])
        packages.setdefault(name, (record, []))[1].append(runtime_file.name)

    notice_lines = [
        "Third-party runtime notices",
        "===========================",
        "",
        "These packages provide DLLs bundled alongside Liminal.",
        "Their license files are included under licenses/.",
        "",
    ]
    entries: list[tuple[str, Path]] = []
    for name, (record, filenames) in sorted(packages.items()):
        notice_lines.extend(
            (
                f"{name} {record['version']}",
                f"License: {record.get('license', 'unknown')}",
                f"Files: {', '.join(sorted(filenames, key=str.casefold))}",
                "",
            )
        )

        source = Path(str(record["link"]["source"]))
        license_root = source / "info" / "licenses"
        if license_root.is_dir():
            license_files = sorted(
                path for path in license_root.rglob("*") if path.is_file()
            )
        else:
            recipe_root = source / "info" / "recipe"
            license_files = sorted(
                path
                for path in recipe_root.rglob("*")
                if path.is_file()
                and path.name.casefold().split(".", maxsplit=1)[0]
                in {"copying", "copyright", "license"}
            )
            license_root = recipe_root
        if not license_files:
            raise RuntimeError(
                f"could not find license files for runtime package {name}"
            )

        for license_file in license_files:
            relative_license = license_file.relative_to(license_root).as_posix()
            entries.append((f"licenses/{name}/{relative_license}", license_file))

    notices = PACKAGE_ROOT / "THIRD_PARTY-NOTICES.txt"
    notices.parent.mkdir(parents=True, exist_ok=True)
    notices.write_text("\n".join(notice_lines), encoding="utf-8", newline="\n")
    entries.append((notices.name, notices))
    return entries


def archive_entries(target: Path) -> list[tuple[str, Path]]:
    if sys.platform != "win32":
        raise RuntimeError("portable packaging is currently implemented for Windows")

    runtime_files = windows_runtime_files(target)
    entries = [(target.name, target)]
    entries.extend((path.name, path) for path in runtime_files)
    entries.append(("LICENSE", PROJECT_ROOT / "LICENSE"))
    entries.extend(runtime_license_entries(runtime_files))
    return sorted(entries)


def write_reproducible_zip(
    archive: Path,
    entries: list[tuple[str, Path]],
) -> None:
    archive.parent.mkdir(parents=True, exist_ok=True)
    temporary = archive.with_suffix(f"{archive.suffix}.tmp")
    temporary.unlink(missing_ok=True)
    try:
        with zipfile.ZipFile(
            temporary,
            mode="w",
            compression=zipfile.ZIP_DEFLATED,
            compresslevel=9,
        ) as output:
            for name, source in entries:
                info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                output.writestr(info, source.read_bytes(), compresslevel=9)
        temporary.replace(archive)
    finally:
        temporary.unlink(missing_ok=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        while chunk := file.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def create_package(output_dir: Path) -> tuple[Path, str]:
    target = build_release()
    architecture = platform.machine().lower().replace("amd64", "x86_64")
    archive = output_dir / (f"liminal-{project_version()}-windows-{architecture}.zip")
    entries = archive_entries(target)
    write_reproducible_zip(archive, entries)
    digest = sha256(archive)
    archive.with_suffix(f"{archive.suffix}.sha256").write_text(
        f"{digest}  {archive.name}\n",
        encoding="utf-8",
    )
    print(f"Packaged {archive} ({len(entries)} files)")
    print(f"SHA-256: {digest}")
    return archive, digest


def escape_batch_argument(path: Path) -> str:
    return str(path).replace("%", "%%")


def install_package(
    archive: Path,
    digest: str,
    install_root: Path,
    bin_dir: Path,
) -> Path:
    bundle = install_root / digest[:16]
    install_root.mkdir(parents=True, exist_ok=True)
    if not bundle.exists():
        with tempfile.TemporaryDirectory(
            prefix=f".{bundle.name}-",
            dir=install_root,
        ) as temporary:
            temporary_path = Path(temporary)
            with zipfile.ZipFile(archive) as source:
                source.extractall(temporary_path)
            temporary_path.replace(bundle)

    bin_dir.mkdir(parents=True, exist_ok=True)
    if sys.platform == "win32":
        launcher = bin_dir / "liminal.cmd"
        executable = bundle / "liminal.exe"
        contents = f'@echo off\r\n"{escape_batch_argument(executable)}" %*\r\n'
    else:
        raise RuntimeError("local installation is currently implemented for Windows")

    temporary_launcher = launcher.with_suffix(f"{launcher.suffix}.tmp")
    temporary_launcher.write_text(contents, encoding="utf-8", newline="")
    temporary_launcher.replace(launcher)
    print(f"Installed {executable}")
    print(f"Launcher: {launcher}")

    path_entries = {
        os.path.normcase(os.path.abspath(entry))
        for entry in os.environ.get("PATH", "").split(os.pathsep)
        if entry
    }
    normalized_bin_dir = os.path.normcase(os.path.abspath(bin_dir))
    if normalized_bin_dir not in path_entries:
        print(f"Note: add {bin_dir} to PATH to invoke 'liminal' directly.")
    return executable


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build and package an isolated, portable Liminal release."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    package_parser = subparsers.add_parser(
        "package",
        help="build a release and create a Scoop-friendly ZIP",
    )
    package_parser.add_argument(
        "--output-dir",
        type=Path,
        default=PROJECT_ROOT / "dist",
        help="archive output directory (default: ./dist)",
    )

    install_parser = subparsers.add_parser(
        "install",
        help="package and install a content-addressed local build",
    )
    install_parser.add_argument(
        "--output-dir",
        type=Path,
        default=PROJECT_ROOT / "dist",
        help="archive output directory (default: ./dist)",
    )
    install_parser.add_argument(
        "--install-root",
        type=Path,
        default=Path.home() / ".local" / "share" / "liminal",
        help="bundle storage directory (default: ~/.local/share/liminal)",
    )
    install_parser.add_argument(
        "--bin-dir",
        type=Path,
        default=Path.home() / ".local" / "bin",
        help="launcher directory (default: ~/.local/bin)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        archive, digest = create_package(args.output_dir.resolve())
        if args.command == "install":
            install_package(
                archive,
                digest,
                args.install_root.expanduser().resolve(),
                args.bin_dir.expanduser().resolve(),
            )
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"package: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
