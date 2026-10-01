#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
bin=$(mktemp /tmp/pwmvid-media-XXXXXX)
media=$(mktemp /tmp/pwmvid-media-src-XXXXXX.c)
trap 'rm -f "$bin" "$media"' EXIT
# Exercise the enabled production media source without altering its guards.
cp src/dfu_media.c "$media"
cc -std=c11 -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Itests/media_mock -Iinclude "$media" src/image.c tests/media_mock/test_media.c -o "$bin"
"$bin"
