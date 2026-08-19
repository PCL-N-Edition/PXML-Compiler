#!/usr/bin/env python3
"""Strict release-flags build plus byte-for-byte deterministic PXB gate."""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess


def run(command: list[str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--use-lld", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    work = args.work.resolve()
    if work in {source, source.parent, work.parent}:
        raise SystemExit(f"refusing unsafe CI work directory: {work}")
    shutil.rmtree(work, ignore_errors=True)

    flags = ["-O3", "-flto=full", "-DNDEBUG", "-fomit-frame-pointer"]
    linker_flags = ["-flto=full"]
    if args.use_lld:
        linker_flags.append("-fuse-ld=lld")
    run(
        [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(work),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_C_COMPILER={args.compiler}",
            f"-DCMAKE_C_FLAGS_RELEASE={' '.join(flags)}",
            f"-DCMAKE_EXE_LINKER_FLAGS_RELEASE={' '.join(linker_flags)}",
            "-DPXML_BUILD_TESTS=ON",
            "-DPXML_WARNINGS_AS_ERRORS=ON",
        ]
    )
    run(["cmake", "--build", str(work)])
    run(["ctest", "--test-dir", str(work), "--output-on-failure"])

    executable = work / ("pxmlc.exe" if os.name == "nt" else "pxmlc")
    first = work / "determinism-a.pxb"
    second = work / "determinism-b.pxb"
    compile_command = [
        str(executable),
        "--full",
        str(source / "samples" / "Hello.pxml"),
        "--component",
        str(source / "samples" / "components" / "ActionCard.pxml"),
        "--predefined-dir",
        str(source / "components" / "predefined"),
        "-D",
        "WINDOWS",
        "--strict",
        "--release",
    ]
    run([*compile_command, "-o", str(first)])
    run([*compile_command, "-o", str(second)])
    first_bytes = first.read_bytes()
    second_bytes = second.read_bytes()
    if first_bytes != second_bytes:
        raise SystemExit("deterministic diff gate failed: repeated PXB outputs differ")
    digest = hashlib.sha256(first_bytes).hexdigest()
    print(f"deterministic diff gate: {len(first_bytes)} bytes, sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
