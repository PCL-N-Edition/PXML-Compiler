#!/usr/bin/env python3
"""Create deterministic per-target release archives and internal diff inputs."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
import tarfile
import tempfile
import zipfile


def files_under(root: pathlib.Path) -> list[pathlib.Path]:
    return sorted(path for path in root.rglob("*") if path.is_file())


def archive_name(root: pathlib.Path, path: pathlib.Path, target: str) -> str:
    return f"pxml-{target}/{path.relative_to(root).as_posix()}"


def make_zip(root: pathlib.Path, output: pathlib.Path, target: str, epoch: int) -> None:
    date = tuple(__import__("time").gmtime(max(epoch, 315532800)))[:6]
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in files_under(root):
            info = zipfile.ZipInfo(archive_name(root, path, target), date_time=date)
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = path.stat().st_mode
            info.external_attr = ((mode & 0xFFFF) | stat.S_IFREG) << 16
            archive.writestr(info, path.read_bytes())


def make_tar_gz(root: pathlib.Path, output: pathlib.Path, target: str, epoch: int) -> None:
    with tempfile.NamedTemporaryFile(suffix=".tar", delete=False) as temporary:
        tar_path = pathlib.Path(temporary.name)
    try:
        with tarfile.open(tar_path, "w", format=tarfile.PAX_FORMAT) as archive:
            for path in files_under(root):
                info = archive.gettarinfo(str(path), archive_name(root, path, target))
                info.uid = 0
                info.gid = 0
                info.uname = "root"
                info.gname = "root"
                info.mtime = epoch
                with path.open("rb") as stream:
                    archive.addfile(info, stream)
        with tar_path.open("rb") as source, output.open("wb") as destination:
            with gzip.GzipFile(filename="", mode="wb", fileobj=destination, mtime=epoch, compresslevel=9) as zipped:
                shutil.copyfileobj(source, zipped)
    finally:
        tar_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--install", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()

    install = args.install.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    extension = ".zip" if args.target.startswith("windows-") else ".tar.gz"
    archive = output / f"pxml-{args.target}{extension}"
    if extension == ".zip":
        make_zip(install, archive, args.target, epoch)
    else:
        make_tar_gz(install, archive, args.target, epoch)

    diff_directory = output.parent / "diff-input" / args.target
    diff_directory.mkdir(parents=True, exist_ok=True)
    executable_suffix = ".exe" if args.target.startswith("windows-") else ""
    tool_names = ["pxml-expand", "pxml-opt", "pxml-compile", "pxmlc"]
    binaries = {
        tool: install / "bin" / f"{tool}{executable_suffix}" for tool in tool_names
    }
    for tool, binary in binaries.items():
        if not binary.is_file():
            raise SystemExit(f"release executable missing: {binary}")
        shutil.copy2(binary, diff_directory / binary.name)

    version = subprocess.run(
        [str(binaries["pxmlc"]), "--version"], check=True, capture_output=True, text=True
    ).stdout.strip()
    compiler_version = subprocess.run(
        [args.compiler, "--version"], check=True, capture_output=True, text=True
    ).stdout.splitlines()[0]
    info = {
        "schemaVersion": 2,
        "target": args.target,
        "compiler": compiler_version,
        "pxmlVersion": version,
        "archive": archive.name,
        "archiveSha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "binarySha256": {
            tool: hashlib.sha256(binary.read_bytes()).hexdigest()
            for tool, binary in binaries.items()
        },
        "optimization": {
            "flags": ["-O3", "-flto=full", "-DNDEBUG", "-fomit-frame-pointer"],
            "lld": not args.target.startswith("macos-"),
            "pgo": {
                "pxml-expand": "pxml-expand.profdata",
                "pxml-opt": "pxml-opt.profdata",
                "pxml-compile": "pxml-compiler.profdata",
                "pxmlc": "merged stage profiles",
            },
        },
    }
    (output / f"pxml-{args.target}.build.json").write_text(
        json.dumps(info, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
