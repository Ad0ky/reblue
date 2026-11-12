#!/usr/bin/env python3
"""Guest memory without `volatile` in the whole generated tree (the plain-memory build, off by default).

The code generator reads and writes guest memory through volatile pointers (REX_LOAD_* / REX_STORE_* in
generated/reblue_pch.h). volatile keeps every access where the guest code has it: the compiler cannot keep a value in
a register, merge two loads of the same word or schedule a load early. The ports of the renut-nx family
(ChanseyIsTheBest's renut_nx_ultimate_base, docs/hand-and-machine-optimising-guide.md, "codegen passes") ship their
generated code without it; masseffect-nx measured +15-20 % fps from that step. This is our own implementation of the
method those ports describe (their tools are GPL-3.0 and this repository is BSD: nothing of theirs is copied):

  - the pch's REX_LOAD_U8..U64 and REX_STORE_U8..U64 read and write through `REBLUE_GUEST_VOLATILE` pointers, defined
    empty (the MMIO macros REX_MM_* keep volatile);
  - every loop header (a label a later `goto` of the same function jumps back to) gets REBLUE_GEN_LOOP_BARRIER(), a
    compiler memory barrier: a guest loop that polls memory another thread or the GPU changes reads it again on every
    iteration (without it GCC hoists the load and the thread spins forever);
  - every flush-mode change (`ctx.fpscr.` statements) is fenced on both sides by a compiler memory barrier: a float
    loaded after the change is converted in the new mode, as with the volatile loads (tools/switch/localize.py does the
    same for the localized copies).

Every barrier is the full form (`asm volatile("" ::: "memory")`): the safe one. Defining REBLUE_GUEST_VOLATILE as
`volatile` (e.g. -DREBLUE_GUEST_VOLATILE=volatile) gives the generated behaviour back without running this again.

Risk: the fifth batch of localized copies corrupted data on the console with plain accesses (docs/SWITCH.md), with
loop barriers and flush fences already in place, and the cause was not found. So this is a build switch
(PLAIN_MEMORY=1, build-switch.ps1 -PlainMemory) for an A/B NRO, never the default, and it needs its own PGO profile.

Runs in host-stage.sh after the localized copies (localize.py reads the tree without these lines). Idempotent;
--undo takes every change back out (host-stage.sh runs it first on every build, so a build without the switch is the
generator's code again).

  plain_memory.py <repo root> [--undo] [--check]
"""
import os
import re
import sys

LABEL = re.compile(r'^(loc_[0-9A-F]{8}):\s*$')
GOTO = re.compile(r'goto (loc_[0-9A-F]{8});')
BARRIER = '\tREBLUE_GEN_LOOP_BARRIER();'
FENCE = 'REBLUE_GEN_FP_FENCE(); '
FENCE_END = ' REBLUE_GEN_FP_FENCE();'
# One statement that reads or sets the guest FPSCR (flush-mode changes, mffs / mtfsf).
FP_LINE = re.compile(r'^(\s*)([^;{}]*\bctx\.fpscr\.\w+\([^;{}]*\);)\s*$')
FENCED = re.compile(r'^(\s*)' + re.escape(FENCE) + r'([^;{}]*\bctx\.fpscr\.\w+\([^;{}]*\);)' + re.escape(FENCE_END) + r'$')
FUNC = re.compile(r'^(DEFINE_REX_FUNC|PPC_FUNC_IMPL)\(')

PCH_MARK = '// re:Blue plain-memory build (tools/switch/plain_memory.py)'
PCH_BLOCK = PCH_MARK + '''
#ifndef REBLUE_GUEST_VOLATILE
#define REBLUE_GUEST_VOLATILE
#endif
#define REBLUE_GEN_LOOP_BARRIER() __asm__ __volatile__("" ::: "memory")
#define REBLUE_GEN_FP_FENCE() __asm__ __volatile__("" ::: "memory")
// end of the plain-memory block
'''
PCH_PREFIXES = ('#define REX_LOAD_U', '#define REX_STORE_U')


def patch_pch(path, undo):
    text = open(path, encoding='utf-8').read()
    lines = text.split('\n')
    changed = 0
    for i, line in enumerate(lines):
        if line.startswith(PCH_PREFIXES):
            new = (line.replace('(REBLUE_GUEST_VOLATILE u', '(volatile u') if undo
                   else line.replace('(volatile u', '(REBLUE_GUEST_VOLATILE u'))
            changed += new != line
            lines[i] = new
    text = '\n'.join(lines)
    if undo:
        start = text.find(PCH_MARK)
        if start >= 0:
            end = text.find('// end of the plain-memory block\n', start)
            text = text[:start] + text[end + len('// end of the plain-memory block\n'):]
            changed += 1
    elif PCH_MARK not in text:
        if '#define REX_LOAD_U8(x)' not in text:
            sys.exit(f'plain_memory: no REX_LOAD_U8 in {path}: the generated pch changed')
        text = text.replace('#define REX_LOAD_U8(x)', PCH_BLOCK + '#define REX_LOAD_U8(x)', 1)
        changed += 1
    if not undo and changed == 0 and '(REBLUE_GUEST_VOLATILE u' not in text:
        sys.exit(f'plain_memory: no "(volatile u" REX_LOAD/REX_STORE macros in {path}: the generated pch changed')
    return text, changed


def process_file(path, undo):
    lines = open(path, encoding='utf-8').read().split('\n')
    # The generator's text first (also what --undo leaves).
    plain = []
    for line in lines:
        if line == BARRIER:
            continue
        m = FENCED.match(line)
        plain.append(m.group(1) + m.group(2) if m else line)
    if undo:
        return plain, plain != lines, 0, 0
    out = []
    barriers = fences = 0
    i = 0
    while i < len(plain):
        if not FUNC.match(plain[i]):
            out.append(plain[i])
            i += 1
            continue
        j = i + 1
        while j < len(plain) and plain[j] != '}':
            j += 1
        body = plain[i + 1:j]
        seen, headers = set(), set()
        for line in body:
            m = LABEL.match(line)
            if m:
                seen.add(m.group(1))
                continue
            for target in GOTO.findall(line):
                if target in seen:
                    headers.add(target)
        out.append(plain[i])
        for line in body:
            m = FP_LINE.match(line)
            if m:
                out.append(m.group(1) + FENCE + m.group(2) + FENCE_END)
                fences += 1
                continue
            out.append(line)
            m = LABEL.match(line)
            if m and m.group(1) in headers:
                out.append(BARRIER)
                barriers += 1
        i = j
    return out, out != lines, barriers, fences


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    undo, check = '--undo' in sys.argv, '--check' in sys.argv
    gen = os.path.join(args[0], 'generated')
    pch = os.path.join(gen, 'reblue_pch.h')
    if not os.path.isfile(pch):
        sys.exit(f'plain_memory: {pch} is missing')
    pch_text, pch_changed = patch_pch(pch, undo)
    files = barriers = fences = 0
    for f in sorted(os.listdir(gen)):
        if not re.match(r'reblue_recomp\.\d+\.cpp$', f):
            continue
        path = os.path.join(gen, f)
        out, changed, b, fc = process_file(path, undo)
        barriers += b
        fences += fc
        if changed:
            files += 1
            if not check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    if pch_changed and not check:
        open(pch, 'w', encoding='utf-8', newline='\n').write(pch_text)
    what = 'taken out' if undo else f'{barriers} loop barriers, {fences} fenced flush-mode changes'
    print(f'plain_memory: {what}; {files} files {"to change" if check else "changed"}'
          f'{", pch changed" if pch_changed else ""}')


if __name__ == '__main__':
    main()
