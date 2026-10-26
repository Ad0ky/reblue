#!/usr/bin/env python3
"""SPIR-V -> GLSL for uam (deko3d), in the form plume's deko3d back end (plume_deko3d.cpp) takes.

    spv_to_dk_glsl.py shader.spv out.glsl            (SPIRV_CROSS=path to spirv-cross)

Used for re:Blue's game shaders (XenosRecomp's HLSL, compiled by DXC as the uniform-buffer variant,
REBLUE_UBO_CONSTANTS, vertex shaders without -fvk-invert-y) and for its own shaders (src/gpu/shaders/hlsl).
SPIRV-Cross writes Vulkan GLSL; then:

- Descriptor heaps (runtime arrays of texture2D/3D/Cube/2DMS... and of sampler) become deko3d texture handles:
  sampler2D(T[t], S[s]) -> sampler2D(dkh(t, s)), and a texture used alone (texelFetch, textureSize...) T[t] ->
  sampler2D(dkh(t, 0u)). dkh is the image id with the sampler id at bit 20 (dkMakeTextureHandle), relative to the
  image and sampler descriptor sets the back end binds where plume's sets are. re:Blue keeps every texture type in one
  descriptor set with unique slots, so one image table serves 2D, 3D and cube.
- Uniform buffers: set 5's bindings keep their numbers (0 vertex, 1 pixel, 2 shared constants); any other constant
  buffer takes the next free binding. A scalar-layout block (SharedConstants, which uam's Mesa 19 front end does not
  have) is read as a std140 uvec4 array with each member at its SPIR-V offset.
- Push constants become a std140 uniform buffer at binding 5, which the back end fills with dkCmdBufPushConstants.
- Storage buffers: consecutive bindings from 0, in (set, binding) order.
- Specialization constants: "#define SPIRV_CROSS_CONSTANT_ID_<n> <default>" under #ifndef, so a variant is the same text
  with its values defined first (the back end's parseSpecDefaults reads these lines).
- Vertex inputs: uam takes locations 0..15; they are renumbered in order and the original locations listed (vin=).
- gl_VertexIndex/gl_InstanceIndex -> gl_VertexID/gl_InstanceID; spvNMin/Max/Clamp -> min/max/clamp.
- The first line after #version is the back end's binding line: // banjo-dk ubo=<mask> ssbo=<mask> tex=<0|1> [vin=...]

Exit status 1 with a message when something is not handled.
"""
import os
import re
import subprocess
import sys

SPIRV_CROSS = os.environ.get('SPIRV_CROSS') or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                             'SPIRV-Cross', 'build', 'spirv-cross')
PUSH_UBO_BINDING = 5
SAMPLER_OF = {
    'texture1D': 'sampler1D', 'texture2D': 'sampler2D', 'texture3D': 'sampler3D', 'textureCube': 'samplerCube',
    'texture2DArray': 'sampler2DArray', 'textureCubeArray': 'samplerCubeArray', 'texture2DMS': 'sampler2DMS',
    'texture2DMSArray': 'sampler2DMSArray', 'utexture2D': 'usampler2D', 'itexture2D': 'isampler2D',
    'utexture2DMS': 'usampler2DMS', 'utexture3D': 'usampler3D',
}
DROPPED = ('GL_EXT_nonuniform_qualifier', 'GL_EXT_scalar_block_layout', 'GL_EXT_spirv_intrinsics',
           'GL_EXT_samplerless_texture_functions')
ALONE_FUNCS = ('texelFetch', 'textureSize', 'textureQueryLevels', 'textureSamples')


class Unsupported(Exception):
    pass


def closing(text, i, open_ch, close_ch):
    depth = 0
    for j in range(i, len(text)):
        if text[j] == open_ch:
            depth += 1
        elif text[j] == close_ch:
            depth -= 1
            if depth == 0:
                return j
    raise Unsupported(f'unbalanced {open_ch}{close_ch}')


def strip_call(text, name):
    """name(x) -> (x), with nested parentheses."""
    out, i, key = [], 0, name + '('
    while True:
        j = text.find(key, i)
        if j < 0 or (j > 0 and (text[j - 1].isalnum() or text[j - 1] == '_')):
            if j < 0:
                out.append(text[i:])
                return ''.join(out)
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(text[i:j])
        e = closing(text, j + len(name), '(', ')')
        out.append('(' + text[j + len(key):e] + ')')
        i = e + 1


def scalar_blocks(text, dis):
    """Scalar-layout uniform blocks -> std140 uvec4 views, each member read at its SPIR-V offset."""
    for m in list(re.finditer(r'layout\(set = (\d+), binding = (\d+), scalar\) uniform (\w+)\n\{\n(.*?)\n\} (\w+);',
                              text, re.S)):
        tname, inst = m.group(3), m.group(5)
        names = {int(i): n for i, n in re.findall(r'OpMemberName %' + tname + r' (\d+) "(\w+)"', dis)}
        offs = {int(i): int(o) for i, o in re.findall(r'OpMemberDecorate %' + tname + r' (\d+) Offset (\d+)', dis)}
        if not names:
            raise Unsupported(f'no SPIR-V member offsets for {tname}')
        types = {n: (t, a) for t, n, a in re.findall(r'^\s+(\w+) (\w+)(\[\d+\])?;$', m.group(4), re.M)}
        by_name = {n: offs[i] for i, n in names.items() if i in offs}
        size = max(by_name.values()) + 64
        vecs = (size + 15) // 16
        raw = f'{inst}_raw'
        decl = (f'layout(set = {m.group(1)}, binding = {m.group(2)}, std140) uniform {tname}\n'
                f'{{\n    uvec4 {raw}[{vecs}];\n}};')
        text = text[:m.start()] + decl + text[m.end():]

        def rewrite(t):
            out, i, key = [], 0, inst + '.'
            while True:
                j = t.find(key, i)
                if j < 0:
                    out.append(t[i:])
                    return ''.join(out)
                out.append(t[i:j])
                mm = re.match(r'(\w+)', t[j + len(key):])
                n = mm.group(1)
                k = j + len(key) + len(n)
                if n not in by_name:
                    raise Unsupported(f'{inst}.{n}: no offset')
                o = by_name[n]
                typ, arr = types.get(n, ('uint', ''))
                v, c = o // 16, (o // 4) % 4
                if arr:
                    e = closing(t, k, '[', ']')
                    inner = rewrite(t[k + 1:e])
                    elem = f'{raw}[{v}u + uint({inner})]'
                    out.append(elem if typ in ('uvec4',) else
                               f'uintBitsToFloat({elem})' if typ == 'vec4' else
                               (lambda: (_ for _ in ()).throw(Unsupported(f'{n}: array of {typ}')))())
                    i = e + 1
                elif typ == 'uvec4':
                    out.append(f'{raw}[{v}]')
                    i = k
                elif typ == 'vec4':
                    out.append(f'uintBitsToFloat({raw}[{v}])')
                    i = k
                elif typ == 'uint':
                    out.append(f'{raw}[{v}][{c}]')
                    i = k
                elif typ == 'int':
                    out.append(f'int({raw}[{v}][{c}])')
                    i = k
                elif typ == 'float':
                    out.append(f'uintBitsToFloat({raw}[{v}][{c}])')
                    i = k
                elif typ in ('vec2', 'uvec2') and c <= 2:
                    pair = f'uvec2({raw}[{v}][{c}], {raw}[{v}][{c + 1}])'
                    out.append(f'uintBitsToFloat({pair})' if typ == 'vec2' else pair)
                    i = k
                else:
                    raise Unsupported(f'{inst}.{n}: {typ} at {o}')
        text = rewrite(text)
    return text


def convert(spv):
    dis = subprocess.run(['spirv-dis', spv], capture_output=True, text=True).stdout
    stage = 'vert' if 'OpEntryPoint Vertex' in dis else 'frag' if 'OpEntryPoint Fragment' in dis else None
    if not stage:
        raise Unsupported('not a vertex or pixel shader')
    g = subprocess.run([SPIRV_CROSS, '--version', '460', '--no-es', '--vulkan-semantics', spv],
                       capture_output=True, text=True)
    if g.returncode:
        raise Unsupported('spirv-cross: ' + g.stderr.strip())
    text = g.stdout

    text = re.sub(r'^spirv_instruction\([^)]*\) \w+ spvN(Min|Max|Clamp)\([^)]*\);\n', '', text, flags=re.M)
    text = re.sub(r'\bspvN(Min|Max|Clamp)\(', lambda m: m.group(1).lower() + '(', text)
    text = strip_call(text, 'nonuniformEXT')
    text = scalar_blocks(text, dis)

    # ---- descriptor heaps -> bindless handles
    heap_re = re.compile(r'^layout\(set = (\d+), binding = (\d+)\) uniform (\w+) (\w+)\[\];\n', re.M)
    heaps = {}
    for m in heap_re.finditer(text):
        typ = m.group(3)
        if typ != 'sampler' and typ not in SAMPLER_OF:
            raise Unsupported(f'heap of {typ}')
        heaps[m.group(4)] = typ
    text = heap_re.sub('', text)
    if re.search(r'^layout\(set = \d+, binding = \d+\) uniform (\w*texture\w*|sampler) ', text, re.M):
        raise Unsupported('a texture or sampler outside a heap')
    samplers = [n for n, t in heaps.items() if t == 'sampler']

    def rewrite_heaps(t):
        out, i = [], 0
        pat = re.compile(r'\b(\w+)\(|\b(\w+)\[')
        while True:
            m = pat.search(t, i)
            if not m:
                out.append(t[i:])
                return ''.join(out)
            if m.group(1) and m.group(1) in SAMPLER_OF.values():
                # samplerND(heap[t], samplers[s])
                ctor = m.group(1)
                e = closing(t, m.end() - 1, '(', ')')
                inner = t[m.end():e]
                mm = re.match(r'\s*(\w+)\[', inner)
                if mm and mm.group(1) in heaps and heaps[mm.group(1)] != 'sampler':
                    te = closing(inner, mm.end() - 1, '[', ']')
                    tex_idx = rewrite_heaps(inner[mm.end():te])
                    rest = inner[te + 1:].strip()
                    sm = re.match(r',\s*(\w+)\[', rest)
                    if not sm or sm.group(1) not in samplers:
                        raise Unsupported(f'{ctor} without a sampler heap')
                    se = closing(rest, sm.end() - 1, '[', ']')
                    smp_idx = rewrite_heaps(rest[sm.end():se])
                    out.append(t[i:m.start()])
                    out.append(f'{ctor}(dkh({tex_idx}, {smp_idx}))')
                    i = e + 1
                    continue
                out.append(t[i:m.end()])
                i = m.end()
                continue
            if m.group(1) and m.group(1) in ALONE_FUNCS:
                mm = re.match(r'\s*(\w+)\[', t[m.end():])
                if mm and mm.group(1) in heaps and heaps[mm.group(1)] != 'sampler':
                    a = m.end() + mm.end() - 1
                    te = closing(t, a, '[', ']')
                    tex_idx = rewrite_heaps(t[a + 1:te])
                    out.append(t[i:m.end()])
                    out.append(f'{SAMPLER_OF[heaps[mm.group(1)]]}(dkh({tex_idx}, 0u))')
                    i = te + 1
                    continue
                out.append(t[i:m.end()])
                i = m.end()
                continue
            if m.group(2) and m.group(2) in heaps:
                raise Unsupported(f'{m.group(2)} used outside a sampler constructor or {ALONE_FUNCS}')
            out.append(t[i:m.end()])
            i = m.end()
    if heaps:
        text = rewrite_heaps(text)

    # ---- uniform buffers
    ubo_mask = 0
    taken = set()

    def ubo_decl(m):
        nonlocal ubo_mask
        s, b = int(m.group(1)), int(m.group(2))
        binding = b if s == 5 else None
        if binding is None or binding in taken:
            binding = next(k for k in range(16) if k not in taken and k != PUSH_UBO_BINDING)
        taken.add(binding)
        ubo_mask |= 1 << binding
        return f'layout(binding = {binding}, std140) uniform'
    for m in re.finditer(r'layout\(set = 5, binding = (\d+), std140\) uniform', text):
        taken.add(int(m.group(1)))
    taken_set5 = set(taken)
    taken.clear()
    # set 5 first, as numbered; the rest after
    text = re.sub(r'layout\(set = (5), binding = (\d+), std140\) uniform', ubo_decl, text)
    taken |= taken_set5
    text = re.sub(r'layout\(set = (\d+), binding = (\d+), std140\) uniform', ubo_decl, text)
    n_push = len(re.findall(r'layout\(push_constant, std430\) uniform', text))
    text = re.sub(r'layout\(push_constant, std430\) uniform',
                  f'layout(binding = {PUSH_UBO_BINDING}, std140) uniform', text)
    if n_push:
        ubo_mask |= 1 << PUSH_UBO_BINDING

    # ---- storage buffers
    ssbo_re = re.compile(r'layout\(set = (\d+), binding = (\d+), std430\) ((?:\w+ )*)buffer')
    found = sorted({(int(m.group(1)), int(m.group(2))) for m in ssbo_re.finditer(text)})
    slot_of = {sb: k for k, sb in enumerate(found)}
    text = ssbo_re.sub(lambda m: f'layout(binding = {slot_of[(int(m.group(1)), int(m.group(2)))]}, std430) '
                                 f'{m.group(3)}buffer', text)
    ssbo_mask = (1 << len(found)) - 1

    # ---- specialization constants
    text = re.sub(r'^layout\(constant_id = (\d+)\) const (\w+) (\w+) = ([^;]+);',
                  lambda m: (f'#ifndef SPIRV_CROSS_CONSTANT_ID_{m.group(1)}\n'
                             f'#define SPIRV_CROSS_CONSTANT_ID_{m.group(1)} {m.group(4)}\n#endif\n'
                             f'const {m.group(2)} {m.group(3)} = {m.group(2)}(SPIRV_CROSS_CONSTANT_ID_{m.group(1)});'),
                  text, flags=re.M)

    # ---- builtins
    text = re.sub(r'\bgl_VertexIndex\b', 'gl_VertexID', text)
    text = re.sub(r'\bgl_InstanceIndex\b', 'gl_InstanceID', text)

    # ---- vertex inputs
    vin = []
    if stage == 'vert':
        in_re = re.compile(r'^layout\(location = (\d+)\) in (\w+) (\w+);', re.M)
        vin = sorted(int(m.group(1)) for m in in_re.finditer(text))
        if len(vin) > 16:
            raise Unsupported(f'{len(vin)} vertex inputs')
        new_of = {old: k for k, old in enumerate(vin)}
        text = in_re.sub(lambda m: f'layout(location = {new_of[int(m.group(1))]}) in {m.group(2)} {m.group(3)};',
                         text)

    for bad in ('spirv_instruction', 'buffer_reference', 'push_constant', 'layout(set =', 'subpassInput',
                'gl_BaseInstance', 'gl_BaseVertex', 'gl_DrawID'):
        if bad in text:
            raise Unsupported(f'{bad} remains')

    lines = [l for l in text.split('\n') if not any(l.startswith('#extension ' + e) for e in DROPPED)]
    if not lines[0].startswith('#version'):
        raise Unsupported('no #version line')
    meta = f'// banjo-dk ubo=0x{ubo_mask:x} ssbo=0x{ssbo_mask:x} tex={1 if heaps else 0}'
    if vin:
        meta += ' vin=' + ','.join(map(str, vin))
    extra = [meta]
    if heaps:
        extra += ['#extension GL_ARB_bindless_texture : require',
                  '#define dkh(t, s) uvec2(uint(t) | (uint(s) << 20u), 0u)']
    lines[1:1] = extra
    return stage, '\n'.join(lines)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        _, text = convert(sys.argv[1])
    except Unsupported as e:
        sys.exit(f'{sys.argv[1]}: {e}')
    with open(sys.argv[2], 'w') as f:
        f.write(text)


if __name__ == '__main__':
    main()
