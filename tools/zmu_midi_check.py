#!/usr/bin/env python3
"""zmu_midi_check.py <capture> <song drawer> [--min-notes N] [--mt32]

Checks the MIDI the game sent through camd.library (tests/auto/MidiCamd:
tools/serial_capture.py's capture of FS-UAE's serial port) against the
test songs (tools/zmu_test_song.py), played by a reference of
port/amiga/musik_camd.c's player:

  - first GM System On (F0 7E 7F 09 01 F7);
  - a new song or a jump: All Notes Off (CC123) on all 16 channels, before
    it explicit note offs for every note still sounding;
  - a new song: 16 x CC7 = 100 scaled by the music volume, then the song's
    events from step 0 (note on 9n, note off 8n, program Cn, CC7 scaled);
  - a jump: the programs and volumes as they stand at the target, then the
    events from there;
  - a volume change: 16 x CC7, each channel's value scaled;
  - the song again from its start at its end (the planet music loops).

The song that plays is found by its first program change on channel 1
(10 + its number). Each part between two All Notes Off bursts is matched
against the song from the place its first notes are found at; every
message must follow in order, every note on must get its note off, and the
times must keep the song's 1/120 s steps.

With --mt32 (-M mt32): first the MT-32's reset (F0 41 10 16 12 7F 00 00 00
01 F7) instead of GM System On; the song's melodic channels in the order
of first use onto channels 2..9, the rhythm on 10; programs through the
table mt32_prog in port/amiga/musik_camd.c (read from there); drum keys
outside 35..76 dropped.

Prints what matched, the first difference, and the timing.
"""
import argparse
import os
import struct
import sys

NAMES = ['AD1', 'AJ1', 'AA1', 'AC1', 'AV1', 'ABATTLE']


def read_capture(path):
    out = []
    for line in open(path):
        p = line.split()
        if len(p) != 2:
            continue
        t = float(p[0])
        for i in range(0, len(p[1]), 2):
            out.append((t, int(p[1][i:i + 2], 16)))
    return out


def decode(stream):
    """(time, message bytes) with running status expanded; SysEx whole"""
    msgs = []
    run = None
    i = 0
    while i < len(stream):
        t, b = stream[i]
        if b == 0xf0:
            j = i + 1
            while j < len(stream) and stream[j][1] != 0xf7:
                j += 1
            msgs.append((t, bytes([x for _, x in stream[i:j + 1]])))
            i = j + 1
            continue
        if b >= 0xf8:
            i += 1
            continue
        if b & 0x80:
            run = b
            i += 1
        if run is None:
            i += 1
            continue
        n = 1 if (run & 0xf0) in (0xc0, 0xd0) else 2
        data = [x for _, x in stream[i:i + n]]
        if len(data) < n:
            break
        msgs.append((t, bytes([run] + data)))
        i += n
    return msgs


def zmu_events(data):
    """[(step, kind, data bytes)] of a ZMU1 file"""
    assert data[:4] == b'ZMU1'
    p = 16
    step = 0
    ev = []
    while p < len(data):
        w = data[p]
        p += 1
        if w == 255:
            w = struct.unpack('>H', data[p:p + 2])[0]
            p += 2
        kind = data[p]
        p += 1
        n = {0: 3, 1: 2, 2: 2, 3: 2, 4: 0, 5: 0}[kind]
        step += w
        ev.append((step, kind, data[p:p + n]))
        p += n
        if kind == 4:
            break
    return ev


def mt32_table(src):
    """mt32_prog[128] from port/amiga/musik_camd.c"""
    text = open(src).read()
    a = text.index('mt32_prog[128] =')
    body = text[text.index('{', a) + 1:text.index('};', a)]
    vals = []
    for line in body.splitlines():
        line = line.split('/*')[0]
        vals += [int(x) for x in line.replace(',', ' ').split()]
    assert len(vals) == 128, len(vals)
    return vals


def channel_map(ev, mt32):
    if not mt32:
        return {ch: ch for ch in range(16)}
    out = {9: 9}
    nxt = 1
    for step, kind, d in ev:
        if kind > 3:
            continue
        ch = d[0] & 15
        if ch != 9 and ch not in out and nxt <= 8:
            out[ch] = nxt
            nxt += 1
    return out


def scaled(v, vol):
    return (v * vol + 64) // 128


def reference(ev, passes=4, mt32=None):
    """the song's messages as the player sends them (CC7 unscaled), over
    several passes (it loops); mt32: the program table (channels mapped,
    drum keys outside 35..76 dropped)"""
    out = []
    base = 0
    cmap = channel_map(ev, mt32 is not None)
    for _ in range(passes):
        end = 0
        for step, kind, d in ev:
            t = base + step
            if kind == 4:
                end = step
                continue
            if kind > 3:
                continue
            oc = cmap.get(d[0] & 15)
            if oc is None:
                continue
            if kind in (0, 1) and mt32 is not None and oc == 9 and not 35 <= (d[1] & 127) <= 76:
                continue
            if kind == 0:
                key, vel = d[1] & 127, d[2] & 127
                out.append((t, bytes([0x90 | oc, key, vel]) if vel else bytes([0x80 | oc, key, 0])))
            elif kind == 1:
                out.append((t, bytes([0x80 | oc, d[1] & 127, 0])))
            elif kind == 2:
                prog = d[1] & 127
                out.append((t, bytes([0xc0 | oc, mt32[prog] if mt32 is not None else prog])))
            elif kind == 3:
                out.append((t, bytes([0xb0 | oc, 7, d[1] & 127])))
        base += end
    return out


def is_burst(msgs, i, ctl):
    """16 x CC ctl on channels 0..15 from msgs[i]"""
    if i + 16 > len(msgs):
        return False
    for ch in range(16):
        m = msgs[i + ch][1]
        if len(m) != 3 or m[0] != 0xb0 | ch or m[1] != ctl:
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('capture')
    ap.add_argument('songs')
    ap.add_argument('--min-notes', type=int, default=50)
    ap.add_argument('--mt32', action='store_true')
    ap.add_argument('--log', help="the game's log (dist/Test/auto.log): its own "
                    "lateness line 'events late ... at most N us' must stay under 5 ms")
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    mt = mt32_table(os.path.join(here, '..', 'port', 'amiga', 'musik_camd.c')) if a.mt32 else None

    songs = {}      # the first program on the first melodic output channel -> song
    for n, name in enumerate(NAMES, 1):
        ev = zmu_events(open(os.path.join(a.songs, name + '.zmu'), 'rb').read())
        songs[mt[10 + n] if mt else 10 + n] = (name, ev)
    first_ch = 1 if a.mt32 else 0

    msgs = decode(read_capture(a.capture))
    ok = True
    print('zmu_midi_check: %d messages captured' % len(msgs))
    if not msgs:
        print('FAIL: nothing came over the serial port')
        return 1

    start_msg = (bytes([0xf0, 0x41, 0x10, 0x16, 0x12, 0x7f, 0, 0, 0, 0x01, 0xf7]) if a.mt32
                 else bytes([0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7]))
    what = 'the MT-32 reset' if a.mt32 else 'GM System On'
    if msgs[0][1] == start_msg:
        print('ok: %s first' % what)
    else:
        print('FAIL: the first message is %s, not %s' % (msgs[0][1].hex(), what))
        ok = False
    if a.mt32:
        if msgs[-1][1] == start_msg:
            print('ok: the MT-32 reset last')
        else:
            print('FAIL: the last message is %s, not the MT-32 reset' % msgs[-1][1].hex())
            ok = False
        msgs = msgs[:-1]
    drum_hi = sum(1 for t, b in msgs if len(b) == 3 and b[0] in (0x89, 0x99) and not 35 <= b[1] <= 76)
    if a.mt32 and drum_hi:
        print('FAIL: %d drum keys outside 35..76 went out' % drum_hi)
        ok = False
    elif not a.mt32 and drum_hi == 0:
        print('FAIL: no drum key 81 went out (General MIDI plays it)')
        ok = False
    else:
        print('ok: drum keys outside 35..76: %d (%s)' % (drum_hi, 'dropped' if a.mt32 else 'played'))

    # parts between All Notes Off bursts
    parts = []
    cur = []
    i = 1
    while i < len(msgs):
        if is_burst(msgs, i, 123):
            parts.append(cur)
            cur = []
            i += 16
            continue
        cur.append(msgs[i])
        i += 1
    parts.append(cur)

    vols = set(range(129))      # the music volumes every CC7 so far allows
    total_notes = 0
    devs = []
    checked = 0
    for pi, part in enumerate(parts):
        # 16 x CC7 bursts carry a (new) volume: channel 16's value is
        # scaled(100) (no song sets channel 16); the rest is the song's
        items = []
        j = 0
        while j < len(part):
            if is_burst(part, j, 7):
                items.append(('VOL', part[j + 15][1][2]))
                j += 16
                continue
            items.append(('MSG', part[j]))
            j += 1
        msgs_p = [m for k, m in items if k == 'MSG']
        notes = [m for m in msgs_p if m[1][0] & 0xf0 in (0x80, 0x90)]
        if not notes:
            for k, v in items:
                if k == 'VOL':
                    vols = {x for x in range(129) if scaled(100, x) == v}
            continue
        prog0 = [m for m in msgs_p if m[1][0] == 0xc0 | first_ch]
        if not prog0 or prog0[0][1][1] not in songs:
            print('FAIL: part %d: %d notes but no song program on channel %d' % (pi, len(notes), first_ch + 1))
            ok = False
            continue
        name, ev = songs[prog0[0][1][1]]
        ref = reference(ev, mt32=mt)

        # where in the song: the first eight note messages, found in the
        # song; the part plays from the first message of that step
        first = [m[1] for m in notes[:8]]
        ref_notes = [r for r, x in enumerate(ref) if x[1][0] & 0xf0 in (0x80, 0x90)]
        start = None
        for q in range(len(ref_notes) - len(first)):
            if [ref[ref_notes[q + z]][1] for z in range(len(first))] == first:
                start = ref_notes[q]
                break
        if start is None:
            print('FAIL: part %d (%s): its first notes %s are not in the song'
                  % (pi, name, ' '.join(f.hex() for f in first)))
            ok = False
            continue
        while start > 0 and ref[start - 1][0] == ref[start][0]:
            start -= 1

        r = start
        sounding = {}
        mism = None
        pairs = []
        tail = False
        for idx, (k, m) in enumerate(items):
            if k == 'VOL':
                vols = {x for x in range(129) if scaled(100, x) == m}
                continue
            t, b = m
            if b[0] & 0xf0 in (0x80, 0x90):
                key = (b[0] & 15, b[1])
                if b[0] & 0xf0 == 0x90:
                    sounding[key] = sounding.get(key, 0) + 1
                    total_notes += 1
                elif sounding.get(key, 0) > 0:
                    sounding[key] -= 1
            if mism or r >= len(ref):
                continue
            want = ref[r][1]
            match = False
            if want[0] & 0xf0 == 0xb0 and b[0] == want[0] and b[1] == 7:
                ok_vols = {x for x in vols if scaled(want[2], x) == b[2]}
                if ok_vols:
                    vols = ok_vols
                    match = True
            elif b == want:
                match = True
            if match:
                pairs.append((t, ref[r][0]))
                r += 1
                continue
            if not pairs and b[0] & 0xf0 in (0xc0, 0xb0):
                continue            # a jump's programs and volumes before the song
            rest = [mm for kk, mm in items[idx:] if kk == 'MSG']
            if all(x[1][0] & 0xf0 == 0x80 for x in rest):
                tail = True         # the explicit offs before the next burst
                continue
            mism = (t, b.hex(), want.hex())
        left = sum(v for v in sounding.values() if v > 0)
        if pairs:
            off = sorted(t - s / 120.0 for t, s in pairs)[len(pairs) // 2]
            devs += [abs((t - s / 120.0) - off) for t, s in pairs]
        checked += 1
        line = 'part %d: %s from step %d, %d messages in order%s' % (
            pi, name, ref[start][0], len(pairs), ', then note offs' if tail else '')
        if mism:
            print('FAIL: %s; then %s at %.3f s where the song has %s' % (line, mism[1], mism[0], mism[2]))
            ok = False
        else:
            print('ok: %s' % line)
        if left:
            print('FAIL: part %d: %d notes without their note off' % (pi, left))
            ok = False

    if checked == 0:
        print('FAIL: no part of the capture matched a song')
        ok = False
    if total_notes < a.min_notes:
        print('FAIL: only %d note ons (expected at least %d)' % (total_notes, a.min_notes))
        ok = False
    if devs:
        devs.sort()
        p95 = devs[int(len(devs) * 0.95)]
        print('timing: %d messages, off the song steps by median %.1f ms, 95 %% %.1f ms, max %.1f ms'
              % (len(devs), devs[len(devs) // 2] * 1000, p95 * 1000, devs[-1] * 1000))
        if p95 > 0.060:
            # the Mac's clock against FS-UAE's: when the host is loaded the
            # emulated clock lags it and the bytes arrive in bursts -- a
            # warning; the player's own E-clock measure (--log) decides
            print("WARN: on the Mac's clock the timing spreads more than 60 ms (95 %): "
                  "FS-UAE lagging the host?")
    if a.log:
        late = None
        for line in open(a.log, encoding='latin-1', errors='replace'):
            if 'MIDI:' in line and 'at most' in line:
                late = int(line.split('at most')[1].split()[0])
        if late is None:
            print('FAIL: no lateness line in %s' % a.log)
            ok = False
        elif late > 5000:
            print('FAIL: the player sent an event %d us late (its own E-clock)' % late)
            ok = False
        else:
            print("ok: the player's own lateness at most %d us" % late)
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
