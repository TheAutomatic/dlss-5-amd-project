#!/usr/bin/env python3
"""Quantise four fragment elements at a time instead of two, in preprocessed GLSL (glslang -E output).

    quad_quant_glsl.py IN OUT

Why: LLPC (the AMD Windows driver's compiler) turns every e4m3 conversion into its own
`s_setreg(MODE)` + `v_mov 0` + `v_cvt_pk_fp8_f32`, and a pair written into a fragment then
costs a shift and an and-or to put its two bytes beside the previous pair's. A four-wide
conversion is one MODE write and two converts, the second writing the high half of the same
dword (op_sel), with nothing to pack (amdllpc, a 16x16 fragment: 16 -> 9 instructions for
eight elements). fswin_t.comp quantises in pair loops of the form

    for (int c = 0; c < 8; c += 2) {
        [const] T v = f(c);                   zero or more declarations
        [const] fe4m3vec2 q = nr_quant_pair(ARG(c));      or nr_quant_pair32, or a plain fe4m3vec2(..)
        DST(c) = q.x;  DST'(c + 1) = q.y;  ...            assignments only
    }

(or the same over four pairs, `for (int c = 0; c < 4; ++c)` writing elements 2 * c and 2 * c + 1)

and this rewrites exactly those (any other body is left alone) to

    for (int c = 0; c < 8; c += 4) {
        declarations for c (names + _qa), then for c + 2 (names + _qb)
        const fe4m3vec4 q = nr_quant_quad(ARG_qa, ARG_qb);    or nr_quant_quad32
        DST(c) = q.x; DST'(c + 1) = q.y;  DST(c + 2) = q.z; DST'(c + 3) = q.w;
    }

nr_quant_quad(a, b) is nr_quant_pair(a), nr_quant_pair(b) element for element (coopmm.glsl,
NR_QUANT_EXPLICIT), so the bytes are the same; only the grouping of the conversions changes.
Runs before unroll_glsl.py.
"""
import re
import sys

# `c += 2` over eight elements (element c), or `++c` over four pairs (element 2 * c).
HEAD = re.compile(r'for\s*\(\s*int\s+(\w+)\s*=\s*0\s*;\s*\1\s*<\s*(8|4)\s*;\s*(\1\s*\+=\s*2|\+\+\s*\1|\1\s*\+\+)\s*\)\s*\{')
QUANT = re.compile(r'^(?:const\s+)?fe4m3vec2\s+(\w+)\s*=\s*(nr_quant_pair32|nr_quant_pair|nr_ffwd_quant|nr_attn_quant|nr_qscale_fast|nr_kscale_fast|fe4m3vec2)\s*\((.*)\)$', re.S)
# The pair quantiser and its four-wide twin (coopmm.glsl, ffwd3_t.comp, attn.comp).
QUAD = {'nr_quant_pair32': 'nr_quant_quad32', 'nr_quant_pair': 'nr_quant_quad', 'fe4m3vec2': 'nr_convert_quad',
        'nr_ffwd_quant': 'nr_ffwd_quant4', 'nr_attn_quant': 'nr_attn_quant4',
        'nr_qscale_fast': 'nr_qscale_fast4', 'nr_kscale_fast': 'nr_kscale_fast4'}
DECL = re.compile(r'^(const\s+)?(\w+)\s+(\w+)\s*=\s*(.*)$', re.S)
ASSIGN = re.compile(r'^(.*\S)\s*=\s*(\w+)\s*\.\s*([xy])$', re.S)


def match_brace(s, i):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '{':
            depth += 1
        elif s[j] == '}':
            depth -= 1
            if depth == 0:
                return j
    raise ValueError('unbalanced brace')


def statements(body):
    """Top-level `;`-separated statements, or None if the body has blocks or preprocessor lines."""
    if '{' in body or '}' in body or '#' in body:
        return None
    out, depth, cur = [], 0, []
    for ch in body:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ';' and depth == 0:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if ''.join(cur).strip():
        return None
    return out


def subst(expr, v, names, suffix, shift):
    """`v` -> (v + shift) and every renamed local -> name+suffix, on identifier boundaries."""
    def rep(m):
        w = m.group(0)
        if expr[:m.start()].rstrip().endswith('.'):
            return w   # a member or swizzle, not a name
        if w == v:
            return '(%s + %d)' % (v, shift) if shift else v
        if w in names:
            return w + suffix
        return w
    return re.sub(r'\b[A-Za-z_]\w*\b', rep, expr)


def rewrite(v, body, step, limit):
    if (step, limit) not in ((2, 8), (1, 4)):
        return None
    st = statements(body)
    if not st:
        return None
    decls, quant, assigns = [], None, []
    for s in st:
        if quant is None:
            m = QUANT.match(s)
            if m:
                quant = m
                continue
            d = DECL.match(s)
            if not d or d.group(2) in ('fe4m3vec2', 'fe4m3vec4'):
                return None
            decls.append(d)
        else:
            a = ASSIGN.match(s)
            if not a or a.group(2) != quant.group(1):
                return None
            assigns.append(a)
    if quant is None or not assigns:
        return None
    q, fn, arg = quant.group(1), quant.group(2), quant.group(3)
    names = {d.group(3) for d in decls}
    out = []
    for suffix, shift in (('_qa', 0), ('_qb', step)):
        for d in decls:
            out.append('%s%s %s = %s;' % (d.group(1) or '', d.group(2), d.group(3) + suffix,
                                          subst(d.group(4), v, names, suffix, shift)))
    quad = QUAD[fn]
    out.append('const fe4m3vec4 %s = %s(%s, %s);' % (
        q, quad, subst(arg, v, names, '_qa', 0), subst(arg, v, names, '_qb', step)))
    for shift, lanes in ((0, {'x': 'x', 'y': 'y'}), (step, {'x': 'z', 'y': 'w'})):
        for a in assigns:
            out.append('%s = %s.%s;' % (subst(a.group(1), v, names, '_qa' if not shift else '_qb', shift),
                                         q, lanes[a.group(3)]))
    return 'for (int %s = 0; %s < %d; %s += %d) {\n' % (v, v, limit, v, 2 * step) + '\n'.join(out) + '\n}'


def args_of(s, i):
    """The comma-separated top-level arguments of the call whose '(' is at s[i], and the index
    after its ')'."""
    depth, cur, out = 0, [], []
    for j in range(i, len(s)):
        ch = s[j]
        if ch in '([':
            depth += 1
            if depth == 1:
                continue
        elif ch in ')]':
            depth -= 1
            if depth == 0:
                out.append(''.join(cur).strip())
                return out, j + 1
        if ch == ',' and depth == 1:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    raise ValueError('unbalanced parentheses')


def half_pair(expr):
    """X, Y when expr is exactly `f16vec2(float16_t(X), float16_t(Y))`, else None."""
    m = re.match(r'f16vec2\s*\(', expr)
    if not m:
        return None
    a, end = args_of(expr, m.end() - 1)
    if expr[end:].strip() or len(a) != 2:
        return None
    xy = []
    for e in a:
        n = re.match(r'float16_t\s*\(', e)
        if not n:
            return None
        inner, end = args_of(e, n.end() - 1)
        if e[end:].strip() or len(inner) != 1:
            return None
        xy.append(inner[0])
    return xy


def direct_f32(src):
    """Quantise f32 values directly instead of through an f16 round trip:
    nr_quant_pair(f16vec2(float16_t(X), float16_t(Y))) -> nr_quant_pair32(vec2(X, Y)) and
    vec2(f16vec2(float16_t(X), float16_t(Y))) -> vec2(X, Y). An f16 X is unchanged by it; for an f32
    X it drops the f32 -> f16 -> f32 rounding that RADV's NIR already deletes (so the Linux
    network never had it), keeping f32 - at least native precision. LLPC keeps the round trip:
    two converts a value (fswin32: 738 pairs). The RADV gate checks every site this touches."""
    out, i, n = [], 0, 0
    pat = re.compile(r'\b(nr_quant_pair|vec2)\s*\(')
    while True:
        m = pat.search(src, i)
        if not m:
            out.append(src[i:])
            break
        if src[m.start() - 1:m.start()] in ('f', 'e', '_') or src[max(0, m.start() - 3):m.start()] == 'f16':
            out.append(src[i:m.end()]); i = m.end(); continue
        a, end = args_of(src, m.end() - 1)
        xy = half_pair(a[0]) if len(a) == 1 else None
        if xy is None:
            out.append(src[i:m.end()]); i = m.end(); continue
        fn = 'nr_quant_pair32' if m.group(1) == 'nr_quant_pair' else 'vec2'
        out.append(src[i:m.start()])
        out.append('%s(vec2(%s, %s))' % (fn, xy[0], xy[1]) if fn != 'vec2' else 'vec2(%s, %s)' % (xy[0], xy[1]))
        i = end
        n += 1
    return ''.join(out), n


def main():
    src = open(sys.argv[1]).read()
    import os
    if os.environ.get('NR_Q32_DIRECT', '1') == '1':
        src, k = direct_f32(src)
        print('quad_quant_glsl: %d f16 round trips dropped' % k)
    out, i, n = [], 0, 0
    while True:
        m = HEAD.search(src, i)
        if not m:
            out.append(src[i:])
            break
        close = match_brace(src, m.end() - 1)
        step = 2 if '+=' in m.group(3) else 1
        new = rewrite(m.group(1), src[m.end():close], step, int(m.group(2)))
        if new is None:
            out.append(src[i:m.end()])
            i = m.end()
            continue
        out.append(src[i:m.start()])
        out.append(new)
        i = close + 1
        n += 1
    open(sys.argv[2], 'w').write(''.join(out))
    print('quad_quant_glsl: %d loops' % n)


if __name__ == '__main__':
    main()
