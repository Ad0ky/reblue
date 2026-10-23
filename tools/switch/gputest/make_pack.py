#!/usr/bin/env python3
"""Shader pack for the GPU test NRO (gputest.cpp): the same game pixel shaders as SPIR-V for NVK and as DKSH for deko3d.

    make_pack.py --hlsl <XenosRecomp HLSL dump> --dxc <dxc> --uam <uam> --out gputest.bin [shader names...]

The HLSL dump is XenosRecomp's (4th argument / its dump directory). Each pixel shader is compiled by DXC as the Switch
build does it (the uniform-buffer variant), then for deko3d through SPIRV-Cross, spv_to_dk_glsl.py and uam. The
vertex shader is the test's own (gputest_vs.hlsl / .glsl). The pack is made from the game's shaders: keep it out of
the repository, on the SD card beside the NRO.

Format (little endian): "RBGT", u32 version 1, u32 count; the vertex shader as {u32 spirv size, spirv, u32 dksh size,
dksh}; then per pixel shader {char name[64], u64 hash, u32 spirv size, spirv, u32 dksh size, dksh}. Each blob is
padded to 4 bytes.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = ['bd_normal_ps', 'bd_toon_ps', 'bd_normal_ps_wind', 'bd_normal_ps_nolight', 'bd_pe_ps_ms_weight',
           'bd_simple2d_ps', 'bd_pe_ps_dof', 'bd_pe_ps_ms_tex', 'bd_shadowmap_ps', 'bd_pe_ps_quoter', 'bd_effect_ps',
           'bd_pe_ps_brightpass']


def run(args, **kw):
    r = subprocess.run(args, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f'{args[0]} failed: {r.stdout}{r.stderr}')
    return r


def blob(data):
    return struct.pack('<I', len(data)) + data + b'\0' * (-len(data) % 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hlsl', required=True)
    ap.add_argument('--dxc', required=True)
    ap.add_argument('--uam', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('names', nargs='*')
    a = ap.parse_args()
    names = a.names or DEFAULT
    env = dict(os.environ)
    dxc_lib = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(a.dxc))), 'lib', 'x64')
    env['LD_LIBRARY_PATH'] = dxc_lib + ':' + env.get('LD_LIBRARY_PATH', '')
    with tempfile.TemporaryDirectory() as t:
        p = lambda n: os.path.join(t, n)
        run([a.dxc, '-T', 'vs_6_0', '-spirv', '-HV', '2021', os.path.join(HERE, 'gputest_vs.hlsl'), '-Fo', p('vs.spv')],
            env=env)
        run([a.uam, '-s', 'vert', '-o', p('vs.dksh'), os.path.join(HERE, 'gputest_vs.glsl')])
        out = [b'RBGT', struct.pack('<II', 1, len(names)),
               blob(open(p('vs.spv'), 'rb').read()), blob(open(p('vs.dksh'), 'rb').read())]
        for n in names:
            src = os.path.join(a.hlsl, n + '.hlsl')
            first = open(src).readline()
            m = re.search(r'hash=0x([0-9A-Fa-f]+)', first)
            if 'pixel' not in first or not m:
                sys.exit(f'{n}: not a pixel shader of the dump')
            run([a.dxc, '-T', 'ps_6_0', '-HV', '2021', '-all-resources-bound', '-spirv', '-fvk-use-dx-layout',
                 '-DREBLUE_RECOMP', '-DREBLUE_UBO_CONSTANTS', src, '-Fo', p(n + '.spv')], env=env)
            run([sys.executable, os.path.join(HERE, '..', 'deko3d', 'spv_to_dk_glsl.py'), p(n + '.spv'),
                 p(n + '.glsl')])
            run([a.uam, '-s', 'frag', '-o', p(n + '.dksh'), p(n + '.glsl')])
            out += [n.encode().ljust(64, b'\0')[:64], struct.pack('<Q', int(m.group(1), 16)),
                    blob(open(p(n + '.spv'), 'rb').read()), blob(open(p(n + '.dksh'), 'rb').read())]
            print(f'{n}: spirv {os.path.getsize(p(n + ".spv"))} B, dksh {os.path.getsize(p(n + ".dksh"))} B')
    open(a.out, 'wb').write(b''.join(out))
    print(f'{a.out}: {len(names)} pixel shaders')


if __name__ == '__main__':
    main()
