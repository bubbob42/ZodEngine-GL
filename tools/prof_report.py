#!/usr/bin/env python3
"""prof_report.py <samples.prof> <program> [--progsyms FILE] [--libfuncs FILE] [--top N]

Where Zod spends a benchmark's time, from a PC-sample file written with
ZOD_PROFILE=<file> (port/zod_prof.cpp, RadeonGL's pcprof sampler;
tools/hw/zod_run.py ZPROF=1 fetches it to build/hw/<tag>.prof). Taken over
from Doom 64's tools/prof_report.py (same author): the program anchor is
zod_prof_start (C++ may not take main's address), names are demangled.

  program     the UNSTRIPPED binary that ran (dist/Doom64/Doom64 of the same
              build: a rebuild shifts every address); its symbols come from
              m68k-amigaos-nm -- globals only: a static function's samples
              go to the global in front of it, so give
  --progsyms  the full function list of that build (tools/fullsyms.py; the
              Makefile writes build/Doom64.syms, the runner keeps a copy as
              build/hw/<tag>.syms)
  --libfuncs  a function list of the minigl.library that ran ("offset name"
              per line, offsets from the start of its code hunk; RadeonGL
              build/map-0.11/minigl-0.11-functions.txt is the released 0.11
              with statics). Without it -- PiStorm3D, or a RadeonGL build
              without a list -- library samples are shown as 1 KB pages
              relative to the glTexCoord2f hook ("lib?+0x1400").

--under F keeps only the samples with program function F at the PC or on
the stack (_P_Ticker: the game logic alone -- D_MiniLoop calls it, not
G_Ticker).

Four tables: the split (program / library / outside / waiting), the hot
functions, the program functions whose calls the library time belongs to
(the first return address into the program on the stack after the PC: the
caller of the GL call, or of the OS call for "outside"; a stack word counts
as a return address only when the program's code before it is a JSR/BSR),
and the program functions by inclusive time (each function counted once per sample if any of its
addresses is on the stack -- stale stack words make this an upper bound).
The stack is read at every 2-byte offset: exec's switch frame ends on a
halfword, so the task's own longwords straddle the sample's longwords.

File: 'PROF', anchor_lib (the library's glTexCoord2f hook), anchor_prog
(the program's main), count, then per sample the task state and N
longwords at its saved stack pointer. The PC is the slot (nearly) always
inside known code with the most distinct values; on the 060 the FPU's
FPIAR sits in front of it and also points into code (RadeonGL memory).
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess

NM = os.environ.get('GCC_NM', os.path.expanduser('~/amiga-gcc6/bin/m68k-amigaos-nm'))
FILT = os.environ.get('GCC_FILT', os.path.expanduser('~/amiga-gcc6/bin/m68k-amigaos-c++filt'))
ANCHOR = '_zod_prof_start'


def demangle(names):
    """C++ names readable (one c++filt run for all); unchanged on failure"""
    try:
        out = subprocess.run([FILT, '-_'], input='\n'.join(names), capture_output=True,
                             text=True).stdout.splitlines()
        if len(out) == len(names):
            return out
    except OSError:
        pass
    return names
TS_WAIT = 4
LIB_WINDOW = 0x200000       # without a function list: "near the hook" = the library


def prog_symbols(path):
    out = subprocess.run([NM, '-n', path], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'tT':
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    return syms


def readable(syms):
    """the same list with demangled names, the anchor kept as it is"""
    names = demangle([nm for o, nm in syms])
    return [(o, nm if raw == ANCHOR else d) for (o, raw), nm, d in
            zip(syms, [n for o, n in syms], names)]


def lib_symbols(path):
    syms = []
    for line in open(path):
        if line.startswith('#') or not line.strip():
            continue
        off, name = line.split()[:2]
        syms.append((int(off, 16), name))
    syms.sort()
    return syms


def code_hunk(path):
    """the bytes of the program's first code hunk (AmigaOS HUNK format)"""
    d = open(path, 'rb').read()
    L = lambda o: struct.unpack('>I', d[o:o + 4])[0]
    assert L(0) == 0x3F3, '%s is no AmigaOS executable' % path
    o = 4
    while L(o):                          # resident library names (none)
        o += 4 + 4 * L(o)
    o += 4
    first, last = L(o + 4), L(o + 8)
    o += 12 + 4 * (last - first + 1)
    while o < len(d):
        t = L(o) & 0x3fffffff
        if t == 0x3E9:                   # HUNK_CODE
            n = L(o + 4) & 0x3fffffff
            return d[o + 8:o + 8 + 4 * n]
        raise SystemExit('%s: unexpected hunk 0x%x before the code' % (path, t))
    raise SystemExit('%s: no code hunk' % path)


def after_call(code, off):
    """True when the program code at off follows a JSR/BSR, i.e. off can be
    a return address (stale data words on the stack mostly cannot)"""
    def w(o):
        return (code[o] << 8 | code[o + 1]) if 0 <= o < len(code) - 1 else -1
    if 0x4E90 <= w(off - 2) <= 0x4E97:                      # jsr (An)
        return True
    if 0x4EA8 <= w(off - 4) <= 0x4EBB or w(off - 4) == 0x6100:   # jsr d16(An)/(d8,An,Xn)/d16(PC), bsr.w
        return True
    if w(off - 6) in (0x4EB9, 0x61FF):                      # jsr abs.l, bsr.l
        return True
    if (w(off - 2) & 0xFF00) == 0x6100 and w(off - 2) & 0xFF not in (0, 0xFF):   # bsr.b
        return True
    return 0x4EB0 <= w(off - 6) <= 0x4EB7 or 0x4EB0 <= w(off - 8) <= 0x4EB7   # jsr with full extension


class Syms:
    def __init__(self, syms):
        self.syms = syms
        self.keys = [s[0] for s in syms]

    def name(self, off):
        i = bisect.bisect_right(self.keys, off) - 1
        return self.syms[i][1] if i >= 0 else '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples')
    ap.add_argument('prog')
    ap.add_argument('--progsyms')
    ap.add_argument('--libfuncs')
    ap.add_argument('--libsize', type=lambda x: int(x, 0), default=0x29130,
                    help='code size of the library (with --libfuncs; default 0.11)')
    ap.add_argument('--slot', type=int, help='the PC slot, when the guess is wrong')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--under', help='only the samples with this program function '
                    'at the PC or on the stack (e.g. _G_Ticker: the game logic alone)')
    ap.add_argument('--addr', help='the PC histogram inside this program function: offsets '
                    'from its start (match them with m68k-amigaos-objdump -dl of the object '
                    'built with -g and the same flags)')
    a = ap.parse_args()

    data = open(a.samples, 'rb').read()
    magic, hook, mainaddr, n = struct.unpack('>4I', data[:16])
    assert magic == 0x50524f46, 'not a profile file'
    assert n > 0, 'no samples'
    rec = (len(data) - 16) // (4 * n)
    samples = [struct.unpack('>%dI' % rec, data[16 + i * rec * 4:16 + (i + 1) * rec * 4])
               for i in range(n)]

    psyms = lib_symbols(a.progsyms) if a.progsyms else prog_symbols(a.prog)
    poff = dict((nm, o) for o, nm in psyms)
    if ANCHOR not in poff:
        raise SystemExit('%s has no %s symbol (stripped?)' % (a.prog, ANCHOR))
    pbase = mainaddr - poff[ANCHOR]
    psyms = readable(psyms)
    psize = psyms[-1][0] + 0x100
    pnames = Syms(psyms)
    code = code_hunk(a.prog)

    lnames = None
    if a.libfuncs:
        lsyms = lib_symbols(a.libfuncs)
        hooks = [nm for o, nm in lsyms if nm.endswith('Hook_GLTexCoord2f')]
        if not hooks:
            raise SystemExit('%s has no ..Hook_GLTexCoord2f: not a RadeonGL list?' % a.libfuncs)
        lbase = hook - dict((nm, o) for o, nm in lsyms)[hooks[0]]
        lnames = Syms(lsyms)

    def in_prog(pc):
        return pbase <= pc < pbase + psize

    def is_return(v):
        return not (v & 1) and in_prog(v) and after_call(code, v - pbase)

    def where(pc):
        if pc & 1:
            return None
        if in_prog(pc):
            return 'prog', pnames.name(pc - pbase)
        if lnames is not None:
            if lbase <= pc < lbase + a.libsize:
                return 'lib', lnames.name(pc - lbase)
        elif abs(pc - hook) < LIB_WINDOW:
            d = (pc - hook) & ~0x3ff
            return 'lib', 'lib?%s0x%x' % ('+' if d >= 0 else '-', abs(d))
        return None

    def stack_words(s, slot):
        """the 32-bit values above the PC at EVERY 2-byte offset: exec's
        switch frame ends on a halfword (MEASURED 2026-11-12, FS-UAE 040 and
        the A4000D's 060), so a longword view splits every return address"""
        raw = struct.pack('>%dI' % (len(s) - slot - 1), *s[slot + 1:])
        return [struct.unpack('>I', raw[o:o + 4])[0] for o in range(0, len(raw) - 3, 2)]

    running = [s for s in samples if s[0] != TS_WAIT]
    waiting = len(samples) - len(running)
    if not running:
        raise SystemExit('%d samples, all with the task waiting' % len(samples))

    slot = a.slot
    if slot is None:
        best = None
        for k in range(1, rec):
            vals = [s[k] for s in running]
            known = sum(1 for v in vals if where(v))
            distinct = len(set(vals))
            if known >= 0.6 * len(vals) and (best is None or distinct > best[1]):
                best = (k, distinct, known)
        if best is None:
            raise SystemExit('no slot holds code addresses in 60% of the samples: try --slot')
        slot = best[0]

    if a.under:
        def under(s):
            w = where(s[slot])
            if w and w[0] == 'prog' and w[1] == a.under:
                return True
            return any(is_return(v) and pnames.name(v - pbase) == a.under
                       for v in stack_words(s, slot))
        kept = [s for s in running if under(s)]
        print('--under %s: %d of %d running samples (%.1f%%)'
              % (a.under, len(kept), len(running), 100.0 * len(kept) / len(running)))
        running = kept
        if not running:
            raise SystemExit('no sample under %s' % a.under)

    flat = collections.Counter()
    side = collections.Counter()
    caller = collections.Counter()
    incl = collections.Counter()
    for s in running:
        w = where(s[slot])
        if w is None:
            w = ('other', 'outside (OS, ROM, other code)')
        flat[w] += 1
        side[w[0]] += 1
        above = stack_words(s, slot)
        if w[0] != 'prog':
            for v in above:
                if is_return(v):
                    caller[(w[0], pnames.name(v - pbase))] += 1
                    break
            else:
                caller[(w[0], '(no program address on the stack)')] += 1
        seen = set()
        if w[0] == 'prog':
            seen.add(w[1])
        for v in above:
            if is_return(v):
                seen.add(pnames.name(v - pbase))
        for f in seen:
            incl[f] += 1

    if a.addr:
        start = [o for o, nm in psyms if nm == a.addr]
        if not start:
            raise SystemExit('no program function %r' % a.addr)
        hist = collections.Counter()
        for s in running:
            pc = s[slot]
            if not (pc & 1) and in_prog(pc) and pnames.name(pc - pbase) == a.addr:
                hist[pc - pbase - start[0]] += 1
        n = sum(hist.values())
        print('--addr %s: %d samples at its own code (%.1f%% of the running samples)'
              % (a.addr, n, 100.0 * n / max(1, len(running))))
        for off, c in sorted(hist.items(), key=lambda kv: -kv[1])[:a.top]:
            print('  +0x%04x %5d  %5.1f%%' % (off, c, 100.0 * c / max(1, n)))
        return

    tot = len(running)
    alln = len(samples)
    print('%d samples (5 ms), %d longwords each, PC slot %d' % (alln, rec - 1, slot))
    print('task waiting %.1f%% of all samples (vsync, Delay, the card)' % (100.0 * waiting / alln))
    print('of the running samples: program %.1f%%, library %.1f%%, outside %.1f%%'
          % (100.0 * side['prog'] / tot, 100.0 * side['lib'] / tot, 100.0 * side['other'] / tot))
    print()
    print('hot functions (share of running samples):')
    for (kind, name), c in flat.most_common(a.top):
        print('  %5.1f%% %6d  %-5s %s' % (100.0 * c / tot, c, kind, name))
    print()
    print('library and outside time by the program function that called it:')
    for (kind, name), c in caller.most_common(a.top // 2):
        print('  %5.1f%% %6d  %-5s %s' % (100.0 * c / tot, c, kind, name))
    print()
    print('program functions, inclusive (on the stack; an upper bound):')
    for name, c in incl.most_common(a.top // 2):
        print('  %5.1f%% %6d  %s' % (100.0 * c / tot, c, name))


if __name__ == '__main__':
    main()
