#!/bin/sh
#
# autotest.sh -- run one test in FS-UAE without anybody at the keyboard.
#
# The autotest machine boots fsuae/local/autotest-System.hdf, a COPY of the
# system hardfile whose S:User-Startup runs DEV:Test/AutoRun when it exists
# (gmake autotest-hdf makes the copy). This script stages tests/auto/<name>
# as DEV:Test/AutoRun, captures the serial port (DBG trace, MuForce hits),
# starts FS-UAE, waits for DEV:Test/AutoDone, then stops the emulator and
# prints what the test wrote and what came over the serial line.
#
# Usage: tools/autotest.sh <config> <name> [timeout-secs]
# Without timeout-secs, a "; LIMIT <secs>" line in the test sets it, else 240.

CONFIG="$1"
NAME="$2"
LIMIT="${3:-}"
FSUAE="${FSUAE:-/opt/homebrew/bin/fs-uae}"
PORT=${AUTOTEST_PORT:-1246}
OUT=build/autotest
mkdir -p "$OUT" dist/Test

if [ ! -f "tests/auto/$NAME" ]; then
    echo "autotest: no tests/auto/$NAME"; exit 2
fi
if [ -z "$LIMIT" ]; then
    LIMIT=$(LC_ALL=C sed -n 's/^; *LIMIT  *\([0-9][0-9]*\).*/\1/p' "tests/auto/$NAME" | head -1)
fi
LIMIT="${LIMIT:-240}"
if lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
    echo "autotest: port $PORT is busy (another autotest running?)"; exit 2
fi

rm -f dist/Test/AutoDone dist/Test/AutoRun dist/Test/AutoRunLean dist/Test/AutoRun.out dist/Test/auto.log "$OUT/$NAME.serial"
# A test marked "; LEAN" runs from the top of User-Startup, before the rest
# of the boot (the autotest disk's AutotestLean hook).
if LC_ALL=C grep -q '^; *LEAN' "tests/auto/$NAME"; then
    cp "tests/auto/$NAME" dist/Test/AutoRunLean
else
    cp "tests/auto/$NAME" dist/Test/AutoRun
fi

"$FSUAE" "$CONFIG" >"$OUT/fs-uae.log" 2>&1 &
EMU=$!

# Connect to the serial port as soon as FS-UAE listens. nc must keep stdin
# open without sending anything, or it exits (or types into the Amiga).
i=0
while ! lsof -nP -iTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; do
    sleep 0.2; i=$((i + 1))
    if [ $i -gt 100 ]; then echo "autotest: FS-UAE did not open the serial port"; kill $EMU; exit 1; fi
done
# Every line gets the host time: in an idle or countdown test the CADENCE
# is the result.
START=$(date +%s)
# SERIAL_RAW=1 (or "; SERIAL_RAW" in the test): the bytes as they come,
# timed, in hex -- MIDI from camd.library's serial driver
# (tools/serial_capture.py, read by tools/zmu_midi_check.py).
if [ -z "$SERIAL_RAW" ] && LC_ALL=C grep -q '^; *SERIAL_RAW' "tests/auto/$NAME"; then
    SERIAL_RAW=1
fi
if [ -n "$SERIAL_RAW" ]; then
    python3 tools/serial_capture.py 127.0.0.1 $PORT "$OUT/$NAME.serial" &
    CAP=$!
else
# (No tr in the pipe: it buffers, and the tail of the trace dies with it.)
( tail -f /dev/null | nc 127.0.0.1 $PORT | while IFS= read -r line; do
      line=$(printf '%s' "$line" | tr -d '\r')
      [ -n "$line" ] && printf '%4ds %s\n' "$(( $(date +%s) - START ))" "$line"
  done > "$OUT/$NAME.serial" ) &
CAP=$!
fi

secs=0
while [ ! -f dist/Test/AutoDone ]; do
    sleep 1; secs=$((secs + 1))
    if ! kill -0 $EMU 2>/dev/null; then echo "autotest: FS-UAE exited"; break; fi
    if [ $secs -ge "$LIMIT" ]; then echo "autotest: TIMEOUT after ${LIMIT}s"; break; fi
done
sleep 3
kill $EMU 2>/dev/null
pkill -f "nc 127.0.0.1 $PORT" 2>/dev/null; pkill -P $CAP 2>/dev/null; kill $CAP 2>/dev/null
wait $EMU 2>/dev/null
rm -f dist/Test/AutoRun dist/Test/AutoRunLean

echo "=== DEV:Test/AutoRun.out ==="
cat dist/Test/AutoRun.out 2>/dev/null | LC_ALL=C tr '\r' '\n'
echo "=== DEV:Test/auto.log ==="
cat dist/Test/auto.log 2>/dev/null | LC_ALL=C tr '\r' '\n'
echo "=== serial ($OUT/$NAME.serial) ==="
if [ -n "$SERIAL_RAW" ]; then
    echo "(raw capture: $(wc -l < "$OUT/$NAME.serial" 2>/dev/null) chunks)"
else
LC_ALL=C tr '\r' '\n' < "$OUT/$NAME.serial" 2>/dev/null
fi
test -f dist/Test/AutoDone
