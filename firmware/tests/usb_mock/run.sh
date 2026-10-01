#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/pwmvid-usb-tests.XXXXXX")
trap 'rm -rf "$BUILD"' EXIT HUP INT TERM
python3 "$ROOT/tests/usb_mock/generate.py" "$BUILD/test_transport.c"
SANITIZERS=""
if [ "${SANITIZE:-1}" = 1 ]; then
    SANITIZERS="-fsanitize=address,undefined -fno-omit-frame-pointer"
fi
# Intentionally split compiler flag variables; paths remain quoted.
# shellcheck disable=SC2086
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror $SANITIZERS \
    -I"$ROOT/include" -I"$ROOT/tests/usb_mock" \
    "$BUILD/test_transport.c" -o "$BUILD/test_transport"
# LeakSanitizer cannot operate under the execution environment's ptrace.
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0" "$BUILD/test_transport"
if [ "${SANITIZE:-1}" = 1 ]; then
    echo "AddressSanitizer and UBSan enabled; LeakSanitizer disabled"
fi
