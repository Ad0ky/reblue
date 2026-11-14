"""Bone row reads whose range check can go when the constants end right after them (make_dk_shaders.py).

XenosRecomp reads a relative vertex constant (skinning's bone rows, c[a0 + k]) as
    mix(vec4(0.0), VertexShaderConstants.exMatrix[min(_488, 195)], bvec4(_488 < 196))
a range check and a four-way select per read; bd_toon_vs_env, the castle street's characters, has 18. When the array
ends exactly at the end of the 4 KiB constant block (exMatrix: offset 960 + 196 rows of 16 bytes = 4096), row 196 is
the first byte past the bound uniform buffer, which the GPU reads as zero: `exMatrix[min(_488, 196)]` gives the same
value (an index under 0 reads the same row both ways). The idea is renut-nx's (docs/deko3d-shader-optimisations.md
2.17, bonerow, there with a zero register); this is our own implementation.

The text gets REBLUE_ROW(array, index, rows) and two definitions after its first line: by default the original
expression (the same code as before), with REBLUE_BONE_ROW_ZERO defined the plain read. plume defines it
(Deko3DOptions::boneRowZero) only where the vertex constants are bound with exactly 4096 bytes
(bd_deko3d_delta_constants).

Measured with uam (flags 431), 2026-11-15: bd_toon_vs_env with its masks folded and ifmerge, 5888 -> 5120 bytes.
"""
import re

BLOCK_BYTES = 4096
_BLOCK = re.compile(r'uniform type_VertexShaderConstants\s*\{(.*?)\}\s*VertexShaderConstants;', re.S)
_MEMBER = re.compile(r'layout\(offset = (\d+)\) vec4 (\w+)\[\d+\];')
_READ = re.compile(r'mix\(vec4\(0\.0\), VertexShaderConstants\.(\w+)\[min\((_\d+), (\d+)\)\], bvec4\(\2 < (\d+)\)\)')
_DEFS = ('#ifdef REBLUE_BONE_ROW_ZERO\n'
         '#define REBLUE_ROW(a, i, n) a[min(i, n)]\n'
         '#else\n'
         '#define REBLUE_ROW(a, i, n) mix(vec4(0.0), a[min(i, (n) - 1)], bvec4(i < (n)))\n'
         '#endif\n')


def rewrite_bone_rows(glsl):
    """The text with the eligible reads as REBLUE_ROW, and how many."""
    block = _BLOCK.search(glsl)
    if not block:
        return glsl, 0
    offsets = {name: int(off) for off, name in _MEMBER.findall(block.group(1))}
    count = 0

    def repl(m):
        nonlocal count
        name, index, last, rows = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        if name not in offsets or last != rows - 1 or offsets[name] + rows * 16 != BLOCK_BYTES:
            return m.group(0)
        count += 1
        return f'REBLUE_ROW(VertexShaderConstants.{name}, {index}, {rows})'

    out = _READ.sub(repl, glsl)
    if count:
        nl = out.index('\n') + 1
        out = out[:nl] + _DEFS + out[nl:]
    return out, count
