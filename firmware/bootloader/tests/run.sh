#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iinclude src/image.c tests/test_image.c -o "$tmp/test_image"
ASAN_OPTIONS=detect_leaks=0 "$tmp/test_image"
