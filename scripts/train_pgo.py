#!/usr/bin/env python3
"""Train three isolated, stage-realistic LLVM instrumentation profiles."""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import tempfile


def run(command: list[str], profile: pathlib.Path) -> None:
    environment = os.environ.copy()
    environment["LLVM_PROFILE_FILE"] = str(profile / "%m-%p.profraw")
    completed = subprocess.run(
        command,
        check=False,
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise SystemExit(
            f"PGO corpus command failed ({completed.returncode}): "
            f"{' '.join(command)}\n{completed.stderr}"
        )


def write_corpus(
    root: pathlib.Path, item_count: int
) -> tuple[pathlib.Path, pathlib.Path, pathlib.Path]:
    frame = root / "Frame.pxml"
    frame.write_text(
        """<?pxml version="1.0"?>
<x:Component xmlns:x="urn:pcl:pxml:x" x:Name="Frame">
  <x:Property Name="Title" Type="string" Required="true"/>
  <Column Class="frame frame" Padding="12" Gap="6">
    <Text Text="{component.Title}"/>
    <x:Content Slot="Header"/>
    <x:Content/>
  </Column>
</x:Component>
""",
        encoding="utf-8",
    )
    templates = root / "Templates.pxml"
    templates.write_text(
        """<?pxml version="1.0"?>
<x:Module xmlns:x="urn:pcl:pxml:x">
  <x:Template Name="DownloadItem">
    <Text Text="{bind Item.Name ?? &quot;unknown&quot;}"/>
  </x:Template>
</x:Module>
""",
        encoding="utf-8",
    )
    page = root / "RealisticPage.pxml"
    lines = [
        '<?pxml version="1.0" strict="true"?>',
        '<Page xmlns="pcl://ui" xmlns:x="urn:pcl:pxml:x" xmlns:local="./Components">',
        '  <x:Import Source="./Templates.pxml"/>',
        '  <x:Const Name="Product" Value="PCL Launcher"/>',
        '  <local:Frame Title="{const Product}">',
        '    <x:Into Slot="Header"><Text Text="Build dashboard"/></x:Into>',
    ]
    for index in range(item_count):
        lines.extend(
            [
                f'    <local:ActionCard Text="Package {index}">',
                f'      <Text Text="{{bind App.Items[{index}].Status ?? &quot;pending&quot;}}"/>',
                '    </local:ActionCard>',
            ]
        )
    lines.extend(
        [
            '    <x:IfBuild Condition="WINDOWS">',
            '      <Button Text="Install" Command="{cmd App.Install}"/>',
            '      <x:Else><Text Text="Portable build"/></x:Else>',
            '    </x:IfBuild>',
            '  </local:Frame>',
            '</Page>',
        ]
    )
    page.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return page, frame, templates


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--expand", type=pathlib.Path, required=True)
    parser.add_argument("--opt", type=pathlib.Path, required=True)
    parser.add_argument("--compile", type=pathlib.Path, required=True)
    parser.add_argument("--source", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--profiles", type=pathlib.Path, required=True)
    parser.add_argument("--rounds", type=int, default=12)
    args = parser.parse_args()

    executables = [args.expand.resolve(), args.opt.resolve(), args.compile.resolve()]
    if not all(executable.is_file() for executable in executables):
        raise SystemExit("one or more instrumented stage executables are missing")
    source = args.source.resolve()
    profile_roots = {
        "expand": args.profiles.resolve() / "expand",
        "opt": args.profiles.resolve() / "opt",
        "compiler": args.profiles.resolve() / "compiler",
    }
    for profile_root in profile_roots.values():
        profile_root.mkdir(parents=True, exist_ok=True)

    action_card = source / "samples" / "components" / "ActionCard.pxml"
    with tempfile.TemporaryDirectory(prefix="pxml-stage-pgo-") as temporary:
        root = pathlib.Path(temporary)
        page, frame, templates = write_corpus(root, 384)
        expanded = root / "real.expanded.pxml"
        optimized = root / "real.optimized.pxir"
        binary = root / "real.pxb"
        for round_index in range(args.rounds):
            symbol = "WINDOWS" if round_index % 3 != 2 else "PORTABLE"
            run(
                [
                    str(executables[0]), str(page), "-o", str(expanded),
                    "--component", str(action_card), "--component", str(frame),
                    "--import", str(templates),
                    "-D", symbol,
                ],
                profile_roots["expand"],
            )
            run(
                [str(executables[1]), str(expanded), "-o", str(optimized)],
                profile_roots["opt"],
            )
            compiler_command = [
                str(executables[2]), str(optimized), "-o", str(binary), "--strict"
            ]
            if round_index % 4 == 3:
                compiler_command.append("--debug")
            run(compiler_command, profile_roots["compiler"])
            if binary.read_bytes()[:4] != b"PXB1":
                raise SystemExit("compiler PGO corpus did not produce a PXB1 binary")

    for stage, profile_root in profile_roots.items():
        count = len(list(profile_root.glob("*.profraw")))
        if count == 0:
            raise SystemExit(f"{stage} training produced no raw profiles")
        print(f"{stage}: {count} isolated raw profiles")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
