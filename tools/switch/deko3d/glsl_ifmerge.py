"""Merge runs of `if (c) { .. } [else { .. }]` on the same condition variable into one if/else (make_dk_shaders.py).

XenosRecomp turns each predicated Xenos instruction into its own if, and spirv-cross writes them one after another:
bd_toon_vs_env, the castle street's characters, has 15 `if (_458)` in a row (a bone weight test) and 15 `if (_617)`.
Each costs the GPU a branch and a join per vertex (divergent: the condition differs between vertices). Merged, the
bodies run in the same order under the same condition, so the shader computes the same values. Our own implementation
of the idea in renut-nx's docs/deko3d-shader-optimisations.md (2.29, ifmerge); nothing of theirs is copied.

A run continues only when:
  - the next if tests the same plain variable (spirv-cross' `_123`), on a line of its own, with its body in braces;
  - only declarations without a value (`vec4 _470;`) stand between the two ifs (they move above the merged if);
  - no merged body assigns the condition variable.
A body ending in break, continue, return or discard is fine: what follows it in the merged body did not run before
either (it was the next if on the same condition). Bodies are merged recursively.

Measured with uam (flags 431) on the pack, 2026-11-15: 3818 ifs merged in 142 shaders, all compile; code -7.9 %,
registers down in 57 shaders, up in 1; bd_toon_vs_env 8192 -> 6912 bytes, 54 -> 53 registers.
"""
import re

_DECL = re.compile(r'^\s*(?:float|int|uint|bool|[biu]?vec[234]|mat[234](?:x[234])?)\s+_\w+;\s*$')
_IF = re.compile(r'^(\s*)if \((_\d+)\)\s*$')


def _block(lines, i):
    """lines[i] is a line holding only '{'. The lines inside, and the index after the matching '}'."""
    if lines[i].strip() != '{':
        return None
    depth = 0
    for j in range(i, len(lines)):
        depth += lines[j].count('{') - lines[j].count('}')
        if depth == 0:
            if lines[j].strip() != '}':
                return None
            return lines[i + 1:j], j + 1
    return None


def _parse_if(lines, i):
    m = _IF.match(lines[i])
    if not m or i + 1 >= len(lines):
        return None
    t = _block(lines, i + 1)
    if t is None:
        return None
    then, k = t
    els = None
    if k < len(lines) and lines[k].strip() == 'else':
        if k + 1 >= len(lines):
            return None
        e = _block(lines, k + 1)
        if e is None:
            return None
        els, k = e
    return m.group(1), m.group(2), then, els, k


def _assigns(cond, bodies):
    pat = re.compile(r'\b%s\s*(?:[-+*/%%&|^]|<<|>>)?=(?!=)' % re.escape(cond))
    return any(pat.search(line) for body in bodies for line in body)


def _merge(lines, stats):
    out, i = [], 0
    while i < len(lines):
        p = _parse_if(lines, i)
        if not p:
            out.append(lines[i])
            i += 1
            continue
        ind, cond, then, els, k = p
        thens, elses, decls = [then], [els or []], []
        if not _assigns(cond, thens + elses):
            while True:
                j, pending = k, []
                while j < len(lines) and _DECL.match(lines[j]):
                    pending.append(lines[j])
                    j += 1
                q = _parse_if(lines, j) if j < len(lines) else None
                if not q or q[1] != cond or _assigns(cond, [q[2], q[3] or []]):
                    break
                decls += pending
                thens.append(q[2])
                elses.append(q[3] or [])
                k = q[4]
                stats[0] += 1
        out += decls
        out += [ind + 'if (' + cond + ')', ind + '{']
        out += _merge([line for b in thens for line in b], stats)
        out.append(ind + '}')
        body_else = [line for b in elses for line in b]
        if any(line.strip() for line in body_else) or els is not None:
            out += [ind + 'else', ind + '{']
            out += _merge(body_else, stats)
            out.append(ind + '}')
        i = k
    return out


def merge_ifs(glsl):
    """The text with the runs merged, and how many ifs were merged into the one before them."""
    stats = [0]
    lines = glsl.split('\n')
    for _ in range(8):
        merged = _merge(lines, stats)
        if merged == lines:
            break
        lines = merged
    return '\n'.join(lines), stats[0]
