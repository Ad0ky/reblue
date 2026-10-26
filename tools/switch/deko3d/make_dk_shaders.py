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

--uam-variants TOOL (tools/switch/deko3d/uam_variants.cpp, built against thirdparty/uam) also compiles every variant the
console can ask for, with the uam flags of --uam-flags, into a DKSH cache in the output (g_deko3dDksh*): each game
shader for every combination of the bits of g_SpecConstants it reads (the text says which: "// reblue-spec0-mask="),
each of re:Blue's own once. The keys are plume's (FNV-1a 64 of the exact text uam gets), so the console compiles none
of them, only what the cache lacks.
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
DKSH_RECORD_MAGIC = 0x48534B44  # "DKSH": [magic][u64 key][u32 size][DKSH], switch_tuning.cpp's cache file records


def spec0_mask(glsl):
    """The bits of constant 0 (g_SpecConstants) the shader reads, or None when it has no constant 0."""
    if '#define SPIRV_CROSS_CONSTANT_ID_0 ' not in glsl:
        return None
    mask = 0
    for m in re.finditer(r'g_SpecConstants & (\d+)u', glsl):
        mask |= int(m.group(1))
    return mask


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def variant_texts(glsl):
    """Every text plume's loadVariant can make of this shader: the default, and constant 0 defined after #version."""
    mask = spec0_mask(glsl)
    texts = [glsl]
    if mask:
        bits = [1 << i for i in range(32) if mask >> i & 1]
        nl = glsl.index('\n') + 1
        for k in range(1, 1 << len(bits)):
            value = sum(b for i, b in enumerate(bits) if k >> i & 1)
            texts.append(glsl[:nl] + f'#define SPIRV_CROSS_CONSTANT_ID_0 {value}u\n' + glsl[nl:])
    return texts


def build_dksh_cache(results, tool, flags, jobs):
    """{key: dksh} for every variant of every shader, compiled by uam_variants in 'jobs' processes."""
    with tempfile.TemporaryDirectory() as t:
        work = []
        for n, (key, (stage, glsl)) in enumerate(sorted(results.items(), key=lambda kv: (kv[0][0], str(kv[0][1])))):
            for v, text in enumerate(variant_texts(glsl)):
                src = os.path.join(t, f'{n}_{v}.{stage}')
                with open(src, 'w', newline='\n') as f:
                    f.write(text)
                work.append((stage, src, src + '.dksh', fnv1a64(text.encode())))
        lists = [work[i::jobs] for i in range(jobs)]
        procs = []
        for i, part in enumerate(lists):
            if not part:
                continue
            jf = os.path.join(t, f'jobs{i}.txt')
            with open(jf, 'w') as f:
                f.writelines(f'{s} {src} {out}\n' for s, src, out, _ in part)
            procs.append(subprocess.Popen([tool, str(flags), jf]))
        bad = [p.wait() for p in procs]
        if any(bad):
            sys.exit('uam_variants failed')
        cache = {}
        for _, _, out, key in work:
            with open(out, 'rb') as f:
                cache[key] = f.read()
        return cache


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
    ap.add_argument('--uam-variants', default='')
    ap.add_argument('--uam-flags', type=int, default=431)
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
                mask = spec0_mask(glsl)
                if mask is not None:
                    glsl += f'\n// reblue-spec0-mask=0x{mask:X}\n'
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
    out += ['};', f'const size_t g_deko3dHostShaderCount = {len(host)};', '']
    dksh_note = ''
    if a.uam_variants:
        cache = build_dksh_cache(results, a.uam_variants, a.uam_flags, a.jobs)
        recs = bytearray()
        for key in sorted(cache):
            recs += struct.pack('<IQI', DKSH_RECORD_MAGIC, key, len(cache[key])) + cache[key]
        dpacked = zlib.compress(bytes(recs), 9)
        out += [f'const unsigned g_deko3dDkshFlags = {a.uam_flags}u;',
                f'const size_t g_deko3dDkshCount = {len(cache)};',
                f'const size_t g_deko3dDkshSize = {len(recs)};',
                f'const size_t g_deko3dDkshCompressedSize = {len(dpacked)};',
                'const unsigned char g_deko3dDkshCompressed[] = {']
        for i in range(0, len(dpacked), 32):
            out.append(','.join(str(x) for x in dpacked[i:i + 32]) + ',')
        out += ['};', '']
        dksh_note = f'; DKSH cache: {len(cache)} variants (uam flags {a.uam_flags}), {len(recs)} bytes, {len(dpacked)} packed'
    else:
        out += ['const unsigned g_deko3dDkshFlags = 0u;', 'const size_t g_deko3dDkshCount = 0;',
                'const size_t g_deko3dDkshSize = 0;', 'const size_t g_deko3dDkshCompressedSize = 0;',
                'const unsigned char g_deko3dDkshCompressed[] = {0};', '']
    out += ['} // namespace bd::gpu', '']
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w') as f:
        f.write('\n'.join(out))
    print(f'{a.out}: {len(game)} game and {len(host)} host shaders, {len(raw)} bytes of blobs, {len(packed)} packed'
          f'{dksh_note}')


if __name__ == '__main__':
    main()
