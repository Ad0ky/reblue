#!/usr/bin/env python3
"""Every shader of the deko3d back end, as one generated C++ source (docs/SWITCH-deko3d.md).

    make_dk_shaders.py --repo . --hlsl-dump out/switch-host/hlsl --dxc <dxc-linux> --out out/switch-host/deko3d_shaders.cpp

- The game's shaders: XenosRecomp's HLSL dump (each file's first line has hash=0x...), compiled by DXC as the Switch
  build's uniform-buffer variant (REBLUE_UBO_CONSTANTS), without -fvk-invert-y (deko3d takes Direct3D's clip space).
- re:Blue's own shaders (src/gpu/shaders/hlsl, the list of cmake/generated.cmake), the same way.
Each goes SPIR-V -> spv_to_dk_glsl.py -> a Deko3DShaderBlob (plume_deko3d.h: "BDK3", stage, GLSL size, DKSH size 0)
with the GLSL after it; uam compiles it on the console (plume's compile thread) the first time a pipeline needs it.

The output holds the blobs zlib-compressed in one array, and two tables: the game shaders by hash, re:Blue's by name
(deko3d_shaders.h). It is made from the game, like shader_cache.cpp: it stays in out/, out of the repository.
--check UAM compiles every result with that uam as well (a host build of thirdparty/uam).
"""
import argparse
import concurrent.futures
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import spv_to_dk_glsl  # noqa: E402

# cmake/generated.cmake's host shaders: (stem, profile, extra dxc arguments)
HOST_SHADERS = [(s, 'vs_6_0', []) for s in ('copy_vs', 'bd_2d_blit_vs', 'imgui_vs')] + \
    [(s, 'ps_6_0', []) for s in ('copy_color_ps', 'copy_depth_ps', 'gamma_correction_ps', 'pfx_occlusion_count_ps',
                                 'bd_2d_blit_ps', 'imgui_ps', 'resolve_msaa_color_2x', 'resolve_msaa_color_4x',
                                 'resolve_msaa_color_8x', 'resolve_msaa_depth_2x', 'resolve_msaa_depth_4x',
                                 'resolve_msaa_depth_8x')] + \
    [(s, 'ps_6_0', ['-D', 'REBLUE_RECOMP']) for s in ('bd_pe_ps_brightpass_clamp', 'bd_pe_ps_ms_bright_clamp')]

DK_STAGE = {'vert': 0, 'frag': 4}  # DkStage_Vertex, DkStage_Fragment


def compile_one(job):
    """(key, hlsl path, profile, extra args, dxc, repo, uam) -> (key, stage, glsl) or (key, None, error)"""
    key, hlsl, profile, extra, dxc, repo, uam = job
    env = dict(os.environ)
    lib = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(dxc))), 'lib', 'x64')
    env['LD_LIBRARY_PATH'] = lib + ':' + env.get('LD_LIBRARY_PATH', '')
    with tempfile.TemporaryDirectory() as t:
        spv = os.path.join(t, 's.spv')
        r = subprocess.run([dxc, '-T', profile, '-HV', '2021', '-all-resources-bound', '-Wno-ignored-attributes',
                            '-spirv', '-fvk-use-dx-layout', '-DREBLUE_UBO_CONSTANTS', '-DREBLUE_RECOMP_DEKO3D',
                            *extra, '-I', repo, hlsl, '-Fo', spv], capture_output=True, text=True, env=env)
        if r.returncode:
            return key, None, 'dxc: ' + (r.stderr or r.stdout).strip()[:600]
        try:
            stage, glsl = spv_to_dk_glsl.convert(spv)
        except spv_to_dk_glsl.Unsupported as e:
            return key, None, f'convert: {e}'
        if uam:
            src = os.path.join(t, 's.glsl')
            with open(src, 'w') as f:
                f.write(glsl)
            u = subprocess.run([uam, '-s', stage, '-o', os.path.join(t, 's.dksh'), src], capture_output=True,
                               text=True)
            if u.returncode:
                return key, None, 'uam: ' + (u.stdout + u.stderr).strip()[:600]
        return key, stage, glsl


def blob(stage, glsl):
    data = glsl.encode()
    return struct.pack('<4I', 0x334B4442, DK_STAGE[stage], len(data), 0) + data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', required=True)
    ap.add_argument('--hlsl-dump', required=True)
    ap.add_argument('--dxc', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--check-uam', default='')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    jobs = []
    for name in sorted(os.listdir(a.hlsl_dump)):
        if not name.endswith('.hlsl'):
            continue
        path = os.path.join(a.hlsl_dump, name)
        with open(path) as f:
            first = f.readline()
        m = re.search(r'(vertex|pixel) shader\s+hash=0x([0-9A-Fa-f]+)', first)
        if not m:
            continue
        jobs.append((('game', int(m.group(2), 16)), path, 'vs_6_0' if m.group(1) == 'vertex' else 'ps_6_0',
                     ['-DREBLUE_RECOMP'],
                     a.dxc, a.repo, a.check_uam))
    hlsl_dir = os.path.join(a.repo, 'src', 'gpu', 'shaders', 'hlsl')
    for stem, profile, extra in HOST_SHADERS:
        jobs.append((('host', stem), os.path.join(hlsl_dir, stem + '.hlsl'), profile, extra, a.dxc, a.repo,
                     a.check_uam))

    results, failures = {}, []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for key, stage, glsl in ex.map(compile_one, jobs):
            if stage is None:
                failures.append((key, glsl))
            else:
                results[key] = (stage, glsl)
    for key, err in failures:
        print(f'deko3d shader {key}: {err}', file=sys.stderr)
    if failures:
        sys.exit(f'{len(failures)} of {len(jobs)} shaders failed')

    raw = bytearray()
    game, host = [], []
    for key in sorted(results, key=lambda k: (k[0], str(k[1]))):
        b = blob(*results[key])
        (game if key[0] == 'game' else host).append((key[1], len(raw), len(b)))
        raw += b
    packed = zlib.compress(bytes(raw), 9)
    out = ['// Generated by tools/switch/deko3d/make_dk_shaders.py: the deko3d back end\'s shaders. Do not edit.',
           '#include "gpu/shaders/deko3d_shaders.h"', '', 'namespace bd::gpu {', '',
           f'const size_t g_deko3dPackSize = {len(raw)};',
           f'const size_t g_deko3dPackCompressedSize = {len(packed)};',
           'const unsigned char g_deko3dPackCompressed[] = {']
    for i in range(0, len(packed), 32):
        out.append(','.join(str(x) for x in packed[i:i + 32]) + ',')
    out += ['};', '', 'const Deko3DGameShader g_deko3dGameShaders[] = {']
    out += [f'    {{0x{h:016X}ull, {o}u, {n}u}},' for h, o, n in sorted(game)]
    out += ['};', f'const size_t g_deko3dGameShaderCount = {len(game)};', '',
            'const Deko3DHostShader g_deko3dHostShaders[] = {']
    out += [f'    {{"{s}", {o}u, {n}u}},' for s, o, n in host]
    out += ['};', f'const size_t g_deko3dHostShaderCount = {len(host)};', '', '} // namespace bd::gpu', '']
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w') as f:
        f.write('\n'.join(out))
    print(f'{a.out}: {len(game)} game and {len(host)} host shaders, {len(raw)} bytes of blobs, {len(packed)} packed')


if __name__ == '__main__':
    main()
