#!/usr/bin/env python3
"""zmu_test_song.py <drawer>: the MIDI test's songs (tests/auto/MidiCamd).

The test install has no songs (they come from the user's Z CD), so the test
brings its own: one ZMU1 file under each name the game asks for (AD1, AJ1,
AA1, AC1, AV1, ABATTLE). They differ in the first program change on
channel 1 (song number 1..6), so tools/zmu_midi_check.py can tell which one
played; otherwise the same 20 s pattern:
  - channel 1: a note every 30 steps (0.25 s), 15 steps long, keys from a
    fixed pseudo-random sequence (no stretch repeats: a jump's place is found);
    a second, overlapping note every 120 steps;
  - channel 2: program 33, CC7 90, long notes of 90 steps;
  - channel 10: drum key 36 every 60 steps, key 38 every 120, key 81 (outside
    the MT-32's rhythm keys) every 240;
  - at step 1200: CC7 64 on channel 1; a wait of 300 steps (the u16 escape)
    before the last note.
Format: tools/cd/xmi2zmu.py.
"""
import os
import struct
import sys

NAMES = ['AD1', 'AJ1', 'AA1', 'AC1', 'AV1', 'ABATTLE']
ON, OFF, PROG, VOL, END = 0, 1, 2, 3, 4


def keys(seed):
    """keys from a fixed pseudo-random sequence: no stretch of the song
    repeats, so the checker finds the place a jump went to"""
    x = seed
    while True:
        x = (x * 1103515245 + 12345) & 0x7fffffff
        yield 48 + (x >> 16) % 30


def events(song_no):
    ev = [(0, PROG, (0, 10 + song_no)), (0, VOL, (0, 100)),
          (0, PROG, (1, 33)), (0, VOL, (1, 90))]
    kg = keys(song_no)
    for i in range(0, 2000, 30):
        k = next(kg)
        ev.append((i, ON, (0, k, 100)))
        ev.append((i + 15, OFF, (0, k)))
        if i % 120 == 0:
            ev.append((i + 5, ON, (0, k + 7, 80)))
            ev.append((i + 100, OFF, (0, k + 7)))
    for i in range(0, 2000, 90):
        ev.append((i, ON, (1, 40 + (i // 90) % 5, 90)))
        ev.append((i + 89, OFF, (1, 40 + (i // 90) % 5)))
    for i in range(0, 2000, 60):
        ev.append((i, ON, (9, 36, 110)))
        ev.append((i + 10, OFF, (9, 36)))
        if i % 120 == 60:
            ev.append((i, ON, (9, 38, 100)))
            ev.append((i + 8, OFF, (9, 38)))
        if i % 240 == 180:
            # a key outside the MT-32's rhythm range 35..76: GM plays it,
            # -M mt32 drops it
            ev.append((i + 2, ON, (9, 81, 90)))
            ev.append((i + 6, OFF, (9, 81)))
    ev.append((1200, VOL, (0, 64)))
    ev.append((2400, ON, (0, 72, 100)))     # after a wait of > 254 steps
    ev.append((2410, OFF, (0, 72)))
    ev.append((2420, END, ()))
    order = {OFF: 0, PROG: 1, VOL: 2, ON: 3, END: 4}
    ev.sort(key=lambda e: (e[0], order[e[1]]))
    return ev


def encode(ev):
    out = bytearray()
    step = 0
    for t, kind, data in ev:
        w = t - step
        step = t
        if w < 255:
            out.append(w)
        else:
            out.append(255)
            out += struct.pack('>H', w)
        out.append(kind)
        out += bytes(data)
    head = b'ZMU1' + struct.pack('>HHII', 120, 0, len(ev), ev[-1][0])
    return head + bytes(out)


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    for n, name in enumerate(NAMES, 1):
        with open(os.path.join(d, name + '.zmu'), 'wb') as f:
            f.write(encode(events(n)))
    print('zmu_test_song: %d songs in %s' % (len(NAMES), d))


if __name__ == '__main__':
    main()
