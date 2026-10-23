#!/usr/bin/env python3
"""re:Blue's game shaders for deko3d: SPIR-V (the uniform-buffer variant) -> GLSL that uam compiles.

    spv_to_dk_glsl.py shader.spv out.glsl        (SPIRV_CROSS=path to spirv-cross, else ./SPIRV-Cross/build/)

SPIRV-Cross writes Vulkan GLSL; then:
- the bindless heaps become deko3d texture handles: sampler2D(heap[t], samplers[s]) -> sampler2D(uvec2(t | s << 20, 0))
  (dkMakeTextureHandle: image id, sampler id at bit 20, relative to the bound descriptor sets), and a texture used
  alone (texelFetch, textureSize) gets sampler 0;
- SharedConstants (scalar layout, which uam's Mesa 19 front end does not have) is read as a std140 uvec4 array with
  each member at its SPIR-V offset;
- set 5's uniform buffers keep their bindings (0 vertex, 1 pixel, 2 shared);
- the spec constant becomes SPEC_CONSTANTS (a variant is the same text with it defined first);
- spvNMin/Max/Clamp (SPIR-V-only) become min/max/clamp; nonuniformEXT and the dropped extensions go.
Prototype, from P0 (docs/SWITCH-deko3d.md): all 141 shaders compile with uam, with no warning.
"""

import re,subprocess,sys,os
spv,out=sys.argv[1],sys.argv[2]
dis=subprocess.run(['spirv-dis',spv],capture_output=True,text=True).stdout
g=subprocess.run([os.environ.get('SPIRV_CROSS', os.path.join(os.path.dirname(os.path.abspath(__file__)),'SPIRV-Cross/build/spirv-cross')),'--version','460','--no-es','--vulkan-semantics',spv],capture_output=True,text=True)
if g.returncode: sys.exit('spirv-cross: '+g.stderr)
s=g.stdout
# member offsets of SharedConstants
names={int(i):n for i,n in re.findall(r'OpMemberName %type_SharedConstants (\d+) "(\w+)"',dis)}
offs={int(i):int(o) for i,o in re.findall(r'OpMemberDecorate %type_SharedConstants (\d+) Offset (\d+)',dis)}
s=re.sub(r'#extension GL_EXT_(nonuniform_qualifier|scalar_block_layout|spirv_intrinsics|samplerless_texture_functions) : require\n','',s)
s='#version 460\n#extension GL_ARB_bindless_texture : require\n'+s.split('\n',1)[1]
s=re.sub(r'spirv_instruction\([^)]*\) \w+ spvN(Min|Max|Clamp)\([^)]*\);\n','',s)
s=re.sub(r'\bspvN(Min|Max|Clamp)\(',lambda m:m.group(1).lower()+'(',s)
s=re.sub(r'nonuniformEXT\(([^()]*)\)',r'\1',s)
# bindless: combined handles
s=re.sub(r'layout\(set = \d, binding = 0\) uniform (texture2D|texture3D|textureCube|sampler) g_\w+DescriptorHeap\[\];\n','',s)
def h(img,smp,kind): return f'{kind}(uvec2(({img}) | (({smp}) << 20u), 0u))'
kinds={'2D':'sampler2D','3D':'sampler3D','Cube':'samplerCube'}
for k,kind in kinds.items():
    s=re.sub(r'sampler'+k+r'\(g_Texture'+k+r'DescriptorHeap\[([^\]]*)\], g_SamplerDescriptorHeap\[([^\]]*)\]\)',lambda m:h(m.group(1),m.group(2),kind),s)
    s=re.sub(r'(texelFetch|textureSize|textureQueryLevels)\(g_Texture'+k+r'DescriptorHeap\[([^\]]*)\]',lambda m:m.group(1)+'('+h(m.group(2),'0u',kind),s)
# SharedConstants block -> raw uvec4 view
s=re.sub(r'layout\(set = 5, binding = 2, scalar\) uniform type_SharedConstants\n\{[^}]*\} SharedConstants;',
  'layout(binding = 2, std140) uniform type_SharedConstants { uvec4 shared_raw[22]; };',s)
types=dict((n,(t,arr)) for t,n,arr in re.findall(r'^\s+(\w+) (\w+)(\[\d+\])?;$', g.stdout.split('type_SharedConstants',1)[1].split('}',1)[0], re.M))
def closing(t,i):
    d=0
    for j in range(i,len(t)):
        if t[j]=='[': d+=1
        elif t[j]==']':
            d-=1
            if d==0: return j
def rewrite_members(t):
    out=[];i=0
    while True:
        j=t.find('SharedConstants.',i)
        if j<0: out.append(t[i:]); return ''.join(out)
        out.append(t[i:j]); m=re.match(r'SharedConstants\.(\w+)',t[j:]); n=m.group(1); k=j+m.end()
        idx=[kk for kk,v in names.items() if v==n][0]; o=offs[idx]; typ,arr=types.get(n,('uint',''))
        if arr:
            e=closing(t,k); inner=rewrite_members(t[k+1:e]); out.append(f'shared_raw[{o//16}u + ({inner})]'); i=e+1
        elif typ in ('uvec4','vec4'):
            out.append(f'shared_raw[{o//16}]'); i=k
        elif typ=='float':
            out.append(f'uintBitsToFloat(shared_raw[{o//16}][{(o//4)%4}])'); i=k
        elif typ=='vec2':
            out.append(f'uintBitsToFloat(uvec2(shared_raw[{o//16}][{(o//4)%4}], shared_raw[{o//16}][{(o//4)%4+1}]))'); i=k
        else:
            out.append(f'shared_raw[{o//16}][{(o//4)%4}]'); i=k
s=rewrite_members(s)
# floats read from the raw view
# other UBOs: drop set
s=re.sub(r'layout\(set = 5, binding = (\d), std140\)',r'layout(binding = \1, std140)',s)
s=re.sub(r'layout\(constant_id = 0\) const uint g_SpecConstants = 0u;','#ifndef SPEC_CONSTANTS\n#define SPEC_CONSTANTS 0u\n#endif\nconst uint g_SpecConstants = SPEC_CONSTANTS;',s)
s=re.sub(r'\b(centroid )?in vec4 in_var_','\\1in vec4 in_var_',s)
open(out,'w').write(s)
left=re.findall(r'g_\w+DescriptorHeap',s)
print('leftover heap refs',len(left))
