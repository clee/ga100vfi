#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh tests/run.sh
sh tests/ep0_mock/run.sh
bin=$(mktemp /tmp/pwmvid-ep0-review-XXXXXX)
trap 'rm -f "$bin"' EXIT
cc -std=c11 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Werror -Wno-int-to-pointer-cast -Itests/media_mock -Iinclude tests/test_ep0_review.c -o "$bin"
ASAN_OPTIONS=detect_leaks=0 "$bin"
if [ "$(uname -s)" = Linux ]; then
    sh tests/media_mock/run.sh
    cc -std=c11 -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -Wall -Wextra -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -Itests/media_mock -Iinclude src/ep0_dfu.c src/dfu_media.c src/image.c tests/test_host_flow.c -o "$bin"
    ASAN_OPTIONS=detect_leaks=0 "$bin"
else
    echo 'Linux fixed-address media mock skipped on this host'
fi
