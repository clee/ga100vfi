#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD="${TMPDIR:-/tmp}/pwmvid-tests-$$"
mkdir -p "$BUILD"
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
for fixture in 0 1; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
        -DPWMVID_USB_UPDATER=1 -DPWMVID_ALLOW_FIXTURE="$fixture" -Ilib/control \
        lib/control/control.c tests/test_control.c -o "$BUILD/test-$fixture"
    "$BUILD/test-$fixture"
done
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
    -DPWMVID_USB_UPDATER=1 -DPWMVID_ALLOW_FIXTURE=1 -Ilib/control lib/control/control.c tests/test_review.c -o "$BUILD/review"
"$BUILD/review"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -Ilib/control tests/test_format.c -o "$BUILD/format"
"$BUILD/format"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -DPWMVID_USB_UPDATER=1 -Ilib/control lib/control/control.c tests/test_dfu.c -o "$BUILD/dfu"
"$BUILD/dfu"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
    -Ilib/sequence lib/sequence/sequence.c tests/test_sequence.c -o "$BUILD/sequence"
"$BUILD/sequence"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
    -Ilib/sequence lib/sequence/sequence.c lib/sequence/sequence_engine.c \
    tests/test_sequence_engine.c -o "$BUILD/sequence-engine"
"$BUILD/sequence-engine"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -O1 -g \
    -Ilib/sequence lib/sequence/sequence.c lib/sequence/sequence_engine.c \
    lib/sequence/sequence_dma.c tests/test_sequence_dma.c -o "$BUILD/sequence-dma"
"$BUILD/sequence-dma"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -pedantic -Wno-pointer-to-int-cast -O1 -g \
    -DPWMVID_SEQUENCE=1 -Itests/sequence_runtime_mock -Ilib/control -Ilib/sequence \
    lib/sequence/sequence.c lib/sequence/sequence_engine.c \
    lib/sequence/sequence_dma.c tests/test_sequence_runtime.c -o "$BUILD/sequence-runtime"
"$BUILD/sequence-runtime"
python3 -m unittest discover -s tests -p 'test_sequence_*.py' -v
python3 -m unittest discover -s tests -p 'test_host.py' -v
python3 -m unittest discover -s tests -p 'test_usb_update_host.py' -v
python3 -m unittest discover -s tests -p 'test_telemetry.py' -v
