#!/usr/bin/env python3
"""Build stage-isolated PGO executables and a merged-profile in-memory driver."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys


STAGES = {
    "pxml-expand": "pxml-expand.profdata",
    "pxml-opt": "pxml-opt.profdata",
    "pxml-compile": "pxml-compiler.profdata",
}


def run(command: list[str], *, env: dict[str, str] | None = None) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, check=True, env=env)


def executable(build: pathlib.Path, name: str) -> pathlib.Path:
    return build / f"{name}{'.exe' if os.name == 'nt' else ''}"


def checked_directory(source: pathlib.Path, path: pathlib.Path, label: str) -> pathlib.Path:
    resolved = path.resolve()
    if resolved == source or resolved == source.parent or resolved.parent == resolved:
        raise SystemExit(f"refusing unsafe {label} directory: {resolved}")
    return resolved


def configure(
    source: pathlib.Path,
    build: pathlib.Path,
    compiler: str,
    c_flags: list[str],
    linker_flags: list[str],
    tests: bool,
) -> None:
    run(
        [
            "cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_C_COMPILER={compiler}",
            f"-DCMAKE_C_FLAGS_RELEASE={' '.join(c_flags)}",
            f"-DCMAKE_EXE_LINKER_FLAGS_RELEASE={' '.join(linker_flags)}",
            f"-DPXML_BUILD_TESTS={'ON' if tests else 'OFF'}",
            "-DPXML_WARNINGS_AS_ERRORS=ON",
        ]
    )


def merge_profiles(profdata: str, raw: pathlib.Path, output: pathlib.Path) -> int:
    profiles = sorted(raw.glob("*.profraw"))
    if not profiles:
        raise SystemExit(f"no raw profiles under {raw}")
    run([profdata, "merge", "-o", str(output), *map(str, profiles)])
    return len(profiles)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--work", type=pathlib.Path, required=True)
    parser.add_argument("--install", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--profdata", default="llvm-profdata")
    parser.add_argument("--use-lld", action="store_true")
    parser.add_argument("--training-rounds", type=int, default=12)
    args = parser.parse_args()

    source = args.source.resolve()
    work = checked_directory(source, args.work, "PGO work")
    install = checked_directory(source, args.install, "install")
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(install, ignore_errors=True)
    generate = work / "generate"
    raw = work / "raw"
    raw.mkdir(parents=True)

    base_flags = ["-O3", "-flto=full", "-DNDEBUG", "-fomit-frame-pointer"]
    linker_flags = ["-flto=full"]
    if args.use_lld:
        linker_flags.append("-fuse-ld=lld")
    generate_flag = "-fprofile-instr-generate"
    configure(
        source, generate, args.compiler,
        [*base_flags, generate_flag], [*linker_flags, generate_flag], False,
    )
    run(["cmake", "--build", str(generate), "--target", *STAGES.keys()])
    run(
        [
            sys.executable, str(source / "scripts" / "train_pgo.py"),
            "--expand", str(executable(generate, "pxml-expand")),
            "--opt", str(executable(generate, "pxml-opt")),
            "--compile", str(executable(generate, "pxml-compile")),
            "--source", str(source), "--profiles", str(raw),
            "--rounds", str(args.training_rounds),
        ]
    )

    profile_counts: dict[str, int] = {}
    profiles: dict[str, pathlib.Path] = {}
    for stage, profile_name in STAGES.items():
        stage_key = "compiler" if stage == "pxml-compile" else stage.removeprefix("pxml-")
        output = work / profile_name
        profile_counts[stage] = merge_profiles(args.profdata, raw / stage_key, output)
        profiles[stage] = output
    merged = work / "pxmlc-merged.profdata"
    run([args.profdata, "merge", "-o", str(merged), *map(str, profiles.values())])

    merged_build = work / "use-pxmlc"
    merged_flag = f"-fprofile-instr-use={merged.as_posix()}"
    configure(
        source, merged_build, args.compiler,
        [
            *base_flags,
            merged_flag,
            "-Wno-profile-instr-unprofiled",
            "-Wno-profile-instr-out-of-date",
        ],
        [*linker_flags, merged_flag], True,
    )
    run(["cmake", "--build", str(merged_build)])
    run(["ctest", "--test-dir", str(merged_build), "--output-on-failure"])
    run(["cmake", "--install", str(merged_build), "--prefix", str(install)])

    for stage, profile in profiles.items():
        stage_build = work / f"use-{stage}"
        use_flag = f"-fprofile-instr-use={profile.as_posix()}"
        configure(
            source, stage_build, args.compiler,
            [
                *base_flags,
                use_flag,
                "-Wno-profile-instr-unprofiled",
                "-Wno-profile-instr-out-of-date",
            ],
            [*linker_flags, use_flag], False,
        )
        run(["cmake", "--build", str(stage_build), "--target", stage])
        shutil.copy2(executable(stage_build, stage), install / "bin" / executable(stage_build, stage).name)

    profile_install = install / "share" / "pxml" / "pgo"
    profile_install.mkdir(parents=True, exist_ok=True)
    for profile in profiles.values():
        shutil.copy2(profile, profile_install / profile.name)
    for name in [*STAGES.keys(), "pxmlc"]:
        run([str(install / "bin" / executable(install / "bin", name).name), "--version"])
    metadata = {
        "schemaVersion": 2,
        "compiler": args.compiler,
        "flags": base_flags,
        "linkerFlags": linker_flags,
        "trainingRounds": args.training_rounds,
        "profiles": {
            stage: {"file": STAGES[stage], "rawProfileCount": profile_counts[stage]}
            for stage in STAGES
        },
        "pxmlcProfile": "merge of the three isolated stage profiles",
    }
    (install / "pgo-build.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
