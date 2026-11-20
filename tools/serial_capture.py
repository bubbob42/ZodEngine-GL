#!/usr/bin/env python3
"""serial_capture.py <host> <port> <out>: FS-UAE's serial port (tcp), binary-safe.

Each chunk received becomes a line '<seconds since connect> <hex bytes>'.
tools/autotest.sh uses it instead of its line-based capture when SERIAL_RAW
is set (the MIDI test: the bytes camd.library's serial driver sends, read by
tools/zmu_midi_check.py). Taken over from Doom 64 for AmigaOS's
tools/serial_capture.py (same author).
"""
import socket
import sys
import time

s = socket.create_connection((sys.argv[1], int(sys.argv[2])))
t0 = time.time()
with open(sys.argv[3], 'w') as f:
    while True:
        b = s.recv(4096)
        if not b:
            break
        f.write('%.3f %s\n' % (time.time() - t0, b.hex()))
        f.flush()
