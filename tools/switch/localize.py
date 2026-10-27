#!/usr/bin/env python3
"""Localized copies of hot recompiled functions (Switch build).

A recompiled function keeps the guest registers it uses in the PPCContext
(ctx.r3, ctx.f1, ctx.v13 ...) and reaches guest memory through volatile
pointers. GCC therefore stores every register it computes back to ctx and,
around each guest store, often reloads it again: the hottest instructions of
bdSceneNodeDrawSingle on the console were a 23-word fill loop that reloaded
ctx.r11 from memory on every iteration. This writes, for each function listed
in src/platform/switch/hot/localized.txt, a copy that runs exactly the same
statements with two differences:

  - the context registers it mentions are C++ locals (l_r3, l_f1, l_v13 ...):
    read from ctx at entry, written back before every call and every return,
    and read again after every call, so callers and callees see exactly the
    context the recompiled function would leave. ctx.fpscr, ctx.lr and the
    rest stay in the context;
  - guest memory is accessed without `volatile` (hot_natives.cpp redefines the
    REX_LOAD/STORE macros), with a compiler barrier at the head of every loop
    (every label a later goto jumps back to), so a load of memory another
    thread writes is never hoisted out of a loop.

Calls are every `NAME(ctx, base);`, REX_CALL_INDIRECT_FUNC(...) and
ppc_trap(...). A function that mentions ctx in any other way is refused
(reported, left to the recompiled code), as is one with a reservation
(lwarx/stwcx.) or setjmp.

The idea is the one bejeweled3-nx and sa2-nx use (their localize_native.py);
this is a separate, smaller implementation for re:Blue's generated code.

The list file: one function per line, `NAME` for a function no host hook
replaces (the copy gets its own hook), `NAME hooked` for one a hook in the
sources already replaces (that hook calls bd::hot::Call_NAME instead of
__imp__NAME; the copy only provides the function). `#` starts a comment.

Runs after tools/switch/direct_calls.py (host-stage.sh), so the copy calls its
callees exactly as the recompiled function does.

Usage: localize.py <repo root> [--check]
"""
import os
import re
import sys

DEF = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
REG = re.compile(r'\bctx\.(r\d+|f\d+|v\d+)\b')
CALL = re.compile(r'(?<![\w:])((?:__imp__)?[A-Za-z_]\w*)\(ctx, base\);')
INDIRECT = re.compile(r'REX_CALL_INDIRECT_FUNC\([^;]*\);')
TRAP = re.compile(r'\bppc_trap\(ctx, base, \d+\);')
LABEL = re.compile(r'^(loc_[0-9A-Fa-f]+):\s*$')
GOTO = re.compile(r'\bgoto (loc_[0-9A-Fa-f]+);')
RETURN = re.compile(r'(?<![\w.])return;')
BY_REF_ARG = re.compile(r'([(,]\s*)l_(\w+)(?=\s*[,)])')


EXTERN_DECL = re.compile(r'^extern (?:void|bool|u32|uint32_t|int) (\w+)\(.*\);$')


def find_externs(gen):
    """The file-scope declarations of the host functions (mid-asm hooks) the
    generated code calls with registers as arguments."""
    decls = {}
    for f in sorted(os.listdir(gen)):
        if f.startswith('reblue_recomp.') and f.endswith('.cpp'):
            for line in open(os.path.join(gen, f), encoding='utf-8'):
                m = EXTERN_DECL.match(line.rstrip('\n'))
                if m:
                    decls.setdefault(m.group(1), line.rstrip('\n'))
    return decls


def find_bodies(gen, names):
    bodies = {}
    for f in sorted(os.listdir(gen)):
        if not (f.startswith('reblue_recomp.') and f.endswith('.cpp')):
            continue
        lines = open(os.path.join(gen, f), encoding='utf-8').read().split('\n')
        i = 0
        while i < len(lines):
            m = DEF.match(lines[i])
            if m and m.group(1) in names:
                j = i + 1
                while lines[j] != '}':
                    j += 1
                bodies[m.group(1)] = lines[i + 1:j]
                i = j
            i += 1
    return bodies


def reg_type(r):
    return 'PPCVRegister' if r[0] == 'v' else 'PPCRegister'


def reg_key(r):
    return ({'r': 0, 'f': 1, 'v': 2}[r[0]], int(r[1:]))


def localize(name, body):
    text = '\n'.join(body)
    for bad, why in (('reserved', 'a reservation (lwarx/stwcx.)'), ('setjmp', 'setjmp'),
                     ('longjmp', 'longjmp'), ('ctx.lr', 'the link register')):
        if bad in text:
            return None, why
    # Everything else that hands ctx over must be a call form this script syncs around.
    rest = REG.sub('', text)
    rest = CALL.sub('', rest)
    rest = INDIRECT.sub('', rest)
    rest = TRAP.sub('', rest)
    rest = re.sub(r'\bctx\.fpscr\b', '', rest)
    for line in rest.split('\n'):
        if re.search(r'\bctx\b', line):
            return None, 'ctx used as ' + line.strip()[:80]

    regs = sorted(set(REG.findall(text)), key=reg_key)
    callees = sorted({m for m in CALL.findall(text)})

    # Loop heads: labels some later goto jumps back to.
    label_line = {}
    for i, line in enumerate(body):
        m = LABEL.match(line)
        if m:
            label_line[m.group(1)] = i
    loop_heads = set()
    for i, line in enumerate(body):
        for target in GOTO.findall(line):
            if target in label_line and label_line[target] <= i:
                loop_heads.add(target)

    out = []
    for line in body:
        m = LABEL.match(line)
        if m:
            out.append(line)
            if m.group(1) in loop_heads:
                out.append('\tREBLUE_LOOP_BARRIER();')
            continue
        if line.lstrip().startswith('//'):
            continue  # the guest disassembly comments
        s = REG.sub(lambda mm: 'l_' + mm.group(1), line)
        s = CALL.sub(lambda mm: '{ @WB@ ' + mm.group(0) + ' @RL@ }', s)
        s = INDIRECT.sub(lambda mm: '{ @WB@ ' + mm.group(0) + ' @RL@ }', s)
        s = TRAP.sub(lambda mm: '{ @WB@ ' + mm.group(0) + ' @RL@ }', s)
        s = RETURN.sub('{ @WB@ return; }', s)
        # A register handed whole to a host function (a mid-asm hook takes
        # PPCRegister&) goes through a temporary copied back after the call:
        # taking the local's own address would keep it in memory everywhere.
        if 'ctx.fpscr.' in s:
            # The recompiled code reads guest memory with volatile loads, which
            # stay on their side of the volatile asm that writes FPCR: a float
            # loaded after a flush-mode change is converted in the new mode.
            # The copy's plain loads get the same fence.
            s = s.replace('ctx.fpscr.', 'REBLUE_FP_FENCE(); ctx.fpscr.', 1).rstrip() + ' REBLUE_FP_FENCE();'
        if BY_REF_ARG.search(s):
            m = re.match(r'^(\s*)(\w+)\((.*)\);\s*$', s)
            if not m:
                return None, 'a register passed by reference in ' + s.strip()[:80]
            args = [a.strip() for a in m.group(3).split(',')]
            temps, back = [], []
            for k, a in enumerate(args):
                am = re.fullmatch(r'l_(\w+)', a)
                if am:
                    t = f'hot_t{k}'
                    temps.append(f'{reg_type(am.group(1))} {t} = {a};')
                    back.append(f'{a} = {t};')
                    args[k] = t
            s = (f'{m.group(1)}{{ ' + ' '.join(temps) + f' {m.group(2)}(' + ', '.join(args) + '); '
                 + ' '.join(back) + ' }')
        out.append(s)
    out.append('\t{ @WB@ }')  # falling off the end

    plan = sync_plan(out, regs)
    exact = plan is not None
    if plan is None:  # forms the analysis does not follow: everything at every sync
        plan = ([set(regs)] * len(out), [set(regs)] * len(out), set(regs))
    wbs, rls, live_in = plan

    def wb(rs):
        return ' '.join(f'ctx.{r} = l_{r};' for r in sorted(rs, key=reg_key))

    def rl(rs):
        return ' '.join(f'l_{r} = ctx.{r};' for r in sorted(rs, key=reg_key))

    final = []
    for i, line in enumerate(out):
        final.append(line.replace('@WB@', wb(wbs[i])).replace('@RL@', rl(rls[i])))

    head = [f'// {name}: {len(regs)} registers localized, {len(callees)} callees, '
            f'{len(loop_heads)} loop heads, ' + ('changed-only syncs' if exact else 'full syncs'),
            f'static void n_{name}(PPCContext& __restrict ctx, uint8_t* base) {{']
    for r in regs:
        if r in live_in:
            head.append(f'\t{reg_type(r)} l_{r} = ctx.{r};')
        else:
            head.append(f'\t{reg_type(r)} l_{r}{{}};')
    return (callees, head + final + ['}']), None


# ---------------------------------------------------------------- sync plan
#
# Which registers each sync must move, from two dataflows over the copy's
# statements (a statement is a line; labels and gotos give the jumps):
#   - dirty (forward, may): the locals that may differ from the context. A
#     write makes a register dirty; a call (unconditional) makes all clean.
#     A write-back before a call or a return moves exactly these.
#   - live (backward, may): a register whose local value may still be read,
#     by the code or by a later write-back (which reads the dirty ones). The
#     reload after a call takes exactly these: one that is not live is never
#     read again before being overwritten, so its stale local is never seen.
# Anything else (a switch, a block over several lines with jumps in it) and
# the caller falls back to moving every register at every sync.

FULL_WRITE = re.compile(
    r'^\s*l_(\w+)\.(?:u64|s64|f64) = (.*);\s*$')
FULL_VSTORE = re.compile(
    r'^\s*simde_mm_store_(?:si128|ps)\(\s*(?:\([^()]*\))?\s*l_(v\d+)\.\w+\s*,(.*)\);\s*$')
WRITE_ANY = re.compile(
    r'\bl_(\w+)(?:\.\w+)?(?:\[[^\]]*\])?\s*(?:=(?!=)|\+=|-=|\*=|/=|\|=|&=|\^=|<<=|>>=)')
STORE_INTO = re.compile(r'simde_mm_store\w*\(\s*(?:\([^()]*\))?\s*l_(\w+)\b')
# A register passed whole to a function (a mid-asm hook takes PPCRegister&).
BY_REF = re.compile(r'\bl_(\w+) = hot_t\d+;')
MENTION = re.compile(r'\bl_([rfv]\d+)\b')


def sync_plan(out, regs):
    regs = set(regs)
    # Group lines into statements: a `for (...)` header with its one-line
    # body; a braced block over several lines (only when nothing in it jumps,
    # returns or calls).
    stmts, idx = [], []
    i = 0
    while i < len(out):
        line = out[i]
        st = line.strip()
        if re.match(r'(switch|case\b|default\s*:|while\b|do\b)', st):
            return None
        if st.startswith('for ('):
            stmts.append(line + '\n' + out[i + 1])
            idx.append([i, i + 1])
            i += 2
            continue
        if st == '{':
            j = i + 1
            depth = 1
            while depth:
                t = out[j].strip()
                depth += t.count('{') - t.count('}') if t in ('{', '}') else 0
                j += 1
            block = out[i:j]
            joined = '\n'.join(block)
            if 'goto' in joined or '@' in joined or LABEL.search(joined) or re.search(r'^\s*loc_', joined, re.M):
                return None
            stmts.append(joined)
            idx.append(list(range(i, j)))
            i = j
            continue
        if st.endswith('{') or st == '}':
            return None
        stmts.append(line)
        idx.append([i])
        i += 1

    n = len(stmts)
    labels = {}
    for k, st in enumerate(stmts):
        m = LABEL.match(st)
        if m:
            labels[m.group(1)] = k

    info = []
    for st in stmts:
        s = st.strip()
        is_call = '@RL@' in s
        is_ret = 'return;' in s
        cond = s.startswith('if')
        targets = GOTO.findall(s)
        for t in targets:
            if t not in labels:
                return None
        uncond_goto = bool(targets) and not cond
        falls = not (uncond_goto or (is_ret and not cond))
        # reads / writes outside the sync placeholders
        body = s.replace('@WB@', '').replace('@RL@', '')
        mentioned = set(MENTION.findall(body))
        writes = set(WRITE_ANY.findall(body)) | set(STORE_INTO.findall(body)) | set(BY_REF.findall(body))
        writes &= regs
        kill = set()
        m = FULL_WRITE.match(body) or FULL_VSTORE.match(body)
        if m and not cond and m.group(1) in regs and ('l_' + m.group(1)) not in m.group(2):
            if not re.search(r'\bl_' + m.group(1) + r'\b', m.group(2)):
                kill = {m.group(1)}
        reads = (mentioned - kill)
        succ = []
        if falls:
            succ.append(None)  # next statement
        succ += [labels[t] for t in targets]
        info.append(dict(call=is_call, ret=is_ret, cond=cond, succ=succ,
                         reads=reads, writes=writes | kill, kill=kill, sync='@WB@' in s))

    def successors(k):
        res = []
        for t in info[k]['succ']:
            if t is None:
                if k + 1 < n:
                    res.append(k + 1)
            else:
                res.append(t)
        return res

    preds = [[] for _ in range(n)]
    for k in range(n):
        for t in successors(k):
            preds[t].append(k)

    # dirty: forward may
    din = [set() for _ in range(n)]
    dout = [set() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for k in range(n):
            new_in = set()
            for p in preds[k]:
                new_in |= dout[p]
            it = info[k]
            if it['call'] and not it['cond']:
                new_out = set(it['writes'])  # nothing else stays dirty past the call
            else:
                new_out = new_in | it['writes']
            if new_in != din[k] or new_out != dout[k]:
                din[k], dout[k] = new_in, new_out
                changed = True

    # live: backward may
    lin = [set() for _ in range(n)]
    lout = [set() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for k in reversed(range(n)):
            new_out = set()
            for t in successors(k):
                new_out |= lin[t]
            it = info[k]
            uses = set(it['reads'])
            if it['sync']:
                uses |= din[k]  # the write-back reads the dirty locals
            if it['call']:
                if it['cond']:
                    new_in = uses | new_out
                else:
                    new_in = uses  # the reload provides everything live after it
            elif it['ret'] and not it['cond']:
                new_in = uses
            else:
                new_in = uses | (new_out - it['kill'])
            if new_in != lin[k] or new_out != lout[k]:
                lin[k], lout[k] = new_in, new_out
                changed = True

    wbs = [set(regs) for _ in out]
    rls = [set(regs) for _ in out]
    for k in range(n):
        it = info[k]
        for line_no in idx[k]:
            wbs[line_no] = din[k] & regs
            rls[line_no] = lout[k] & regs
    return wbs, rls, lin[0] if n else set()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = os.path.abspath(sys.argv[1])
    gen = os.path.join(root, 'generated')
    listing = os.path.join(root, 'src', 'platform', 'switch', 'hot', 'localized.txt')
    wanted = []
    for raw in open(listing, encoding='utf-8'):
        line = raw.split('#', 1)[0].split()
        if line:
            wanted.append((line[0], len(line) > 1 and line[1] == 'hooked'))
    bodies = find_bodies(gen, {n for n, _ in wanted})

    decls, defs, hooks, report = set(), [], [], []
    for name, hooked in wanted:
        if name not in bodies:
            report.append(f'{name}: not found in generated/')
            if hooked:
                decls.add('__imp__' + name)
                hooks.append(f'REBLUE_HOT_CALLER_ORIGINAL({name})')
            continue
        result, why = localize(name, bodies[name])
        if result is None:
            report.append(f'{name}: left to the recompiled code ({why})')
            if hooked:  # its hook still calls bd::hot::Call_NAME
                decls.add('__imp__' + name)
                hooks.append(f'REBLUE_HOT_CALLER_ORIGINAL({name})')
            continue
        callees, lines = result
        decls.update(callees)
        decls.add('__imp__' + name)
        defs.append('\n'.join(lines))
        if hooked:
            hooks.append(f'REBLUE_HOT_CALLER({name})')
        else:
            hooks.append(f'REBLUE_HOT_HOOK({name})')
        report.append(f'{name}: localized' + (' (called by its hook)' if hooked else ''))

    text = ['// Generated by tools/switch/localize.py from generated/reblue_recomp.*.cpp. Do not edit.',
            '// clang-format off']
    text += [f'REX_EXTERN({d});' for d in sorted(decls)]
    externs = find_externs(gen)
    body_text = '\n'.join(defs)
    text += [line for name, line in sorted(externs.items()) if re.search(r'\b' + name + r'\(', body_text)]
    text += defs
    text.append('#define REBLUE_HOT_LIST(X) \\')
    text += [f'  X({h.split("(")[1][:-1]}) \\' for h in hooks if 'ORIGINAL' not in h]
    text.append('')
    text += hooks
    text.append('')
    content = '\n'.join(text)

    out_dir = os.path.join(gen, 'hot')
    out = os.path.join(out_dir, 'localized.inc')
    old = open(out, encoding='utf-8').read() if os.path.exists(out) else None
    if '--check' not in sys.argv and old != content:
        os.makedirs(out_dir, exist_ok=True)
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(content)
    for line in report:
        print('localize:', line)
    print(f'localize: {len(defs)} of {len(wanted)} functions localized'
          + ('' if old != content else ' (unchanged)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
