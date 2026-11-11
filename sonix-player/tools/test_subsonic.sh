#!/bin/sh
# Run from any directory; keep the test binary outside the source tree.
set -eu
cd "$(dirname "$0")/.."
test_binary=$(mktemp /tmp/sonix-subsonic-test.XXXXXX)
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
cc -Wall -Wextra -Werror -g -ffunction-sections -Wl,--gc-sections -fsanitize=address,undefined -I. \
    tools/test_subsonic.c src/system/streaming/subsonic.c \
    src/system/core/json.c src/system/core/md5.c -o "$test_binary"
"$test_binary"
