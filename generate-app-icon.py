#!/usr/bin/env python3
"""Build a modern macOS .icns file from one square PNG.

This avoids relying on iconutil's iconset compiler, which is unavailable or
broken on some Command Line Tools-only macOS installations.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile


REPRESENTATIONS = (
    ("icp4", 16),
    ("ic11", 32),
    ("icp5", 32),
    ("ic12", 64),
    ("ic07", 128),
    ("ic13", 256),
    ("ic08", 256),
    ("ic14", 512),
    ("ic09", 512),
    ("ic10", 1024),
)


def png_representation(source: Path, size: int, destination: Path) -> bytes:
    subprocess.run(
        [
            "/usr/bin/sips",
            "-z",
            str(size),
            str(size),
            str(source),
            "--out",
            str(destination),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    return destination.read_bytes()


def build_icns(source: Path, output: Path) -> None:
    if not source.is_file():
        raise SystemExit(f"找不到图标源文件：{source}")

    chunks: list[bytes] = []
    with tempfile.TemporaryDirectory(prefix="codex-hud-icon-") as temp_dir:
        temp_path = Path(temp_dir)
        for index, (chunk_type, size) in enumerate(REPRESENTATIONS):
            png = png_representation(source, size, temp_path / f"{index}-{size}.png")
            chunks.append(
                chunk_type.encode("ascii") + struct.pack(">I", len(png) + 8) + png
            )

    body = b"".join(chunks)
    payload = b"icns" + struct.pack(">I", len(body) + 8) + body
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_output = output.with_suffix(output.suffix + ".tmp")
    temporary_output.write_bytes(payload)
    os.replace(temporary_output, output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    build_icns(args.input.resolve(), args.output.resolve())
    print(f"图标已生成：{args.output}")


if __name__ == "__main__":
    main()
