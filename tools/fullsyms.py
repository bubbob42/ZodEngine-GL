#!/usr/bin/env python3
"""fullsyms.py <program> <object drawer> <out> -- every function of the
program, statics included, as "offset name" lines (offsets as
m68k-amigaos-nm prints them for the program), for tools/prof_report.py
--progsyms.

The linked AmigaOS binary keeps only global symbols, so a PC sample in a
static function is charged to the global in front of it: the statics at the
top of port/zod_gl.cpp (batch_begin, tex_get, ...) showed up as
"zod_copy_init()", the last global of fastcopy.o before them. The objects
still have their statics. Each object is placed by its global functions that
the binary has too: all of them must give the same base, else the object is
left out (and named). Without a link map, as Doom 64's fullsyms.py needs.

The object drawer is the build's obj-68040 or obj-68060, the one of the
binary (build/amiga-t5/obj-68060 for build/amiga-t5/zod_060).
"""
import os
import subprocess
import sys

NM = os.environ.get('GCC_NM', os.path.expanduser('~/amiga-gcc6/bin/m68k-amigaos-nm'))


def nm(path):
    out = subprocess.run([NM, '-n', path], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'tT':
            syms.append((int(p[0], 16), p[1], p[2]))
    return syms


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    prog, objdir, outpath = sys.argv[1:]
    final = nm(prog)
    gaddr = dict((name, addr) for addr, kind, name in final if kind == 'T')
    out = dict((addr, name) for addr, kind, name in final)
    placed = 0
    skipped = []
    for root, dirs, files in os.walk(objdir):
        for fn in files:
            if not fn.endswith('.o'):
                continue
            path = os.path.join(root, fn)
            syms = nm(path)
            bases = set(gaddr[name] - addr for addr, kind, name in syms
                        if kind == 'T' and name in gaddr)
            if len(bases) != 1:
                if syms:
                    skipped.append((os.path.relpath(path, objdir), len(bases)))
                continue
            base = bases.pop()
            for addr, kind, name in syms:
                out.setdefault(base + addr, name)
            placed += 1
    with open(outpath, 'w') as f:
        for addr in sorted(out):
            f.write('%08x %s\n' % (addr, out[addr]))
    print('%s: %d functions, %d objects placed' % (outpath, len(out), placed))
    for p, n in skipped:
        print('  not placed: %s (%s)' % (p, 'no global in the binary' if n == 0 else 'bases disagree'))


if __name__ == '__main__':
    main()
