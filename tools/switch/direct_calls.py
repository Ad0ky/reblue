#!/usr/bin/env python3
"""Direct calls between recompiled functions that have no hook (Switch build).

Every recompiled function NAME is a weak alias of __imp__NAME (DEFINE_REX_FUNC in
generated/reblue_pch.h), so that a hook (REX_HOOK, REX_HOOK_RAW, REX_STUB...) can
replace it at link time. The generated code always calls NAME, and GCC never
inlines a weak function, in the same file or with LTO, since a hook could
replace it. This rewrites `NAME(ctx, base);` as `__imp__NAME(ctx, base);` where
NAME has no hook: the same function, but one the compiler can now inline. The
idea and the method are nfsmw-nx's (tools/llamadas_directas.py there).

A name counts as hooked when it appears as an identifier anywhere in re:Blue's
sources, the SDK's sources and headers, or the manifest, or when its address
(82xxxxxx) appears there. That is more than strictly needed and errs on the
safe side: a hooked function always keeps the plain call. The indirect-call
table (reblue_init.cpp, reblue_register.cpp) is not touched, so calls through
it still reach the hooks.

The rewrite is idempotent and reversible: every call site gets the form it
should have now, so a hook added later turns its call sites back to plain
calls on the next run. Files are only written when their content changes, so
the build stays incremental. --undo restores every plain call.

Usage: direct_calls.py <repo root> [--undo] [--check-objects <defs.tsv>]
"""
import os
import re
import sys

IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
ADDRESS = re.compile(r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])')
DECLARE = re.compile(r'DECLARE_REX_FUNC\(([A-Za-z_][A-Za-z0-9_]*)\)')
SKIP_EXT = ('.a', '.o', '.obj', '.png', '.jpg', '.bin', '.dds', '.ttf', '.otf', '.zip', '.ico',
            '.spv', '.dxil', '.ipk', '.xex', '.ogg', '.wav')


def hooked_names(root):
    sources = [os.path.join(root, 'src'),
               os.path.join(root, 'thirdparty', 'rexglue-sdk', 'src'),
               os.path.join(root, 'thirdparty', 'rexglue-sdk', 'include')]
    files = [os.path.join(root, f) for f in os.listdir(root) if f.endswith('.toml')]
    for top in sources:
        for d, _, fs in os.walk(top):
            files += [os.path.join(d, f) for f in fs if not f.lower().endswith(SKIP_EXT)]
    names = set()
    for path in files:
        try:
            text = open(path, encoding='utf-8', errors='ignore').read()
        except OSError:
            continue
        names.update(IDENT.findall(text))
        for a in ADDRESS.findall(text):
            names.add('sub_' + a.upper())
    return names


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = os.path.abspath(sys.argv[1])
    undo = '--undo' in sys.argv
    gen = os.path.join(root, 'generated')

    functions = set()
    for f in os.listdir(gen):
        if f.startswith('reblue_funcs.') and f.endswith('.h'):
            functions.update(DECLARE.findall(open(os.path.join(gen, f), encoding='utf-8').read()))
    if not functions:
        print('direct_calls: no recompiled functions found in generated/', file=sys.stderr)
        return 1

    # A game function that shares its name with an import (DECLARE_REX_FUNC(__imp__NAME) as well):
    # there `__imp__NAME(ctx, base);` may be the import call, so its call sites are never touched.
    twins = {n for n in functions if '__imp__' + n in functions}
    hooked = set() if undo else hooked_names(root)
    direct = set() if undo else functions - hooked - twins

    if '--check-objects' in sys.argv:
        # defs.tsv: "<symbol>\t<object>" for every strong symbol of a compiled object. A
        # function defined outside the recompiled code is a hook and must keep its plain call.
        tsv = sys.argv[sys.argv.index('--check-objects') + 1]
        defined_elsewhere = {line.split('\t')[0] for line in open(tsv, encoding='utf-8')
                             if '/reblue_recomp.dir/' not in line}
        wrong = sorted(direct & defined_elsewhere)
        if wrong:
            print('direct_calls: these have a hook but would get direct calls:', ' '.join(wrong[:50]),
                  file=sys.stderr)
            return 1

    call = re.compile(r'(?<![\w])(__imp__)?([A-Za-z_][A-Za-z0-9_]*)\(ctx, base\);')

    def fix(m):
        name = m.group(2)
        if name not in functions or name in twins:
            return m.group(0)  # an import thunk or something else: leave it alone
        return ('__imp__' if name in direct else '') + name + '(ctx, base);'

    files = changed = 0
    for f in sorted(os.listdir(gen)):
        if not (f.startswith('reblue_recomp.') and f.endswith('.cpp')):
            continue
        path = os.path.join(gen, f)
        text = open(path, encoding='utf-8').read()
        new = call.sub(fix, text)
        files += 1
        if new != text:
            with open(path, 'w', encoding='utf-8', newline='\n') as out:
                out.write(new)
            changed += 1

    print(f'direct_calls: {len(functions)} functions, {len(functions) - len(direct)} hooked or named in the '
          f'sources, {len(direct)} called directly; {changed} of {files} files rewritten'
          + (' (undo)' if undo else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
