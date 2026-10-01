#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
bin=$(mktemp /tmp/pwmvid-ep0-XXXXXX)
trap 'rm -f "$bin"' EXIT
cc -std=c11 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Wno-overflow -Itests/ep0_mock -Iinclude src/ep0_dfu.c tests/ep0_mock/test_ep0.c -o "$bin"
ASAN_OPTIONS=detect_leaks=0 "$bin"
