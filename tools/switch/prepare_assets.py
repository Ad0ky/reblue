#!/usr/bin/env python3
"""Fill assets/ with the two game inputs the build needs.

  python3 tools/switch/prepare_assets.py <game folder>

<game folder> is the re:Blue install's game\\ folder (or any folder with the
disc 1 files): it must hold default.xex and pack/!necessity.ipk.

  assets/default.xex      the codegen input (reblue_manifest.toml)
  assets/shaders/*.vso    the 152 Xenos shaders XenosRecomp turns into the
  assets/shaders/*.pso    compiled-in SPIR-V cache

The shaders sit zlib-compressed in !necessity.ipk (bd_system\\fx\\). The IPK1
layout is the one src/vfs/mounts.cpp reads. Nothing here is committed:
assets/.gitignore ignores everything.
"""
import shutil
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "assets"
SHADER_EXTS = (".vso", ".pso")


def ipk_records(path: Path):
    data = path.read_bytes()
    magic, _align, count, _total = struct.unpack_from("<4sIII", data, 0)
    if magic != b"IPK1":
        raise SystemExit(f"{path}: not an IPK1 archive")
    for i in range(count):
        base = 0x10 + i * 0x60
        if base + 0x60 > len(data):
            break
        name = data[base:base + 0x40].split(b"\0", 1)[0].decode("latin-1")
        compressed, zsize, offset, size = struct.unpack_from("<IIII", data, base + 0x40)
        if not name or not size:
            continue
        blob = data[offset:offset + zsize]
        yield name, (zlib.decompress(blob) if compressed else blob), size


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    game = Path(sys.argv[1])
    xex = game / "default.xex"
    ipk = game / "pack" / "!necessity.ipk"
    for p in (xex, ipk):
        if not p.is_file():
            raise SystemExit(f"missing {p}")

    ASSETS.mkdir(exist_ok=True)
    if not (ASSETS / "default.xex").is_file() or (ASSETS / "default.xex").stat().st_size != xex.stat().st_size:
        shutil.copyfile(xex, ASSETS / "default.xex")
    out = ASSETS / "shaders"
    out.mkdir(exist_ok=True)
    n = 0
    for name, body, size in ipk_records(ipk):
        if not name.lower().endswith(SHADER_EXTS):
            continue
        if len(body) != size:
            raise SystemExit(f"{name}: inflated to {len(body)} bytes, record says {size}")
        (out / name.replace("\\", "/").rsplit("/", 1)[-1]).write_bytes(body)
        n += 1
    print(f"assets/default.xex + {n} shaders in assets/shaders")
    return 0 if n else 1


if __name__ == "__main__":
    sys.exit(main())
