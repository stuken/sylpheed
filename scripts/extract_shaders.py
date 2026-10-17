#!/usr/bin/env python3
"""Gather Project Sylpheed's Xenos shader containers for XenosRecomp.

Sylpheed's shaders come from three places:

- hidden/resource3d/Common.xpr, an XPR pack whose USER resources are the
  game's .xpu/.xvu shaders and .fxobj effects (146 containers);
- a few containers in default.xex's .rdata (movie YUV conversion and 2D
  passes), which sits LZX-compressed and encrypted on disc;
- shaders the game compiles at run time from HLSL embedded in default.xex
  (the debug font and primitive drawing), through D3DXCompileShader.

The build can read the first directly. The other two are captured at run
time: the native renderer writes every shader it has no recompiled entry for
to user/shaders (sylpheed_shader_dump), and any directory of such dumps can be
passed here. XenosRecomp scans every file it is given for containers, so
resources and dumps are copied as they are:

    <out_dir>/xpr_<name>    each USER resource of Common.xpr
    <out_dir>/dump_<file>   each .vso/.pso from the dump directories

shaders.csv lists what was copied, and is what the build depends on.

Usage: extract_shaders.py <game_root> <out_dir> [dump_dir ...]
"""
import csv
import shutil
import struct
import sys
from pathlib import Path

XPR_MAGICS = (b"XPR2", b"XPR5")
CONTAINER_REV_0E = 0x102A0E00


def xpr_user_resources(data: bytes):
    """Yields (name, bytes) for every USER resource of an XPR pack."""
    if data[:4] not in XPR_MAGICS:
        raise SystemExit("Common.xpr is not an XPR pack")
    _header_size, _data_size, count = struct.unpack_from(">III", data, 4)
    for i in range(count):
        kind, offset, size, name_offset = struct.unpack_from(">4sIII", data, 16 + i * 16)
        if kind != b"USER":
            continue
        # Offsets count from the end of the 12-byte file header.
        name_start = 12 + name_offset
        name = data[name_start: data.index(b"\0", name_start)].decode("latin-1")
        yield name, data[12 + offset: 12 + offset + size]


def count_containers(blob: bytes) -> int:
    n = 0
    for i in range(0, len(blob) - 4, 4):
        (word,) = struct.unpack_from(">I", blob, i)
        if word & 0xFFFFFFFE == CONTAINER_REV_0E:
            n += 1
    return n


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    game_root = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    dump_dirs = [Path(p) for p in sys.argv[3:] if p]

    xpr = game_root / "hidden" / "resource3d" / "Common.xpr"
    if not xpr.is_file():
        raise SystemExit(f"{xpr} not found: copy the disc's hidden/ folder into the game root")

    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    rows = []
    for name, blob in xpr_user_resources(xpr.read_bytes()):
        containers = count_containers(blob)
        if not containers:
            continue
        file = f"xpr_{name}"
        (out_dir / file).write_bytes(blob)
        rows.append([file, "Common.xpr", containers])

    for dump_dir in dump_dirs:
        if not dump_dir.is_dir():
            continue
        for path in sorted(list(dump_dir.glob("*.vso")) + list(dump_dir.glob("*.pso"))):
            file = f"dump_{path.name}"
            shutil.copyfile(path, out_dir / file)
            rows.append([file, str(dump_dir), 1])

    with open(out_dir / "shaders.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file", "source", "containers"])
        w.writerows(rows)

    xpr_count = sum(r[2] for r in rows if r[1] == "Common.xpr")
    dumps = sum(1 for r in rows if r[1] != "Common.xpr")
    print(f"{xpr_count} containers from Common.xpr, {dumps} dumped shaders -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
