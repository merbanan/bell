#!/bin/sh
# Native tests for the two pieces of this firmware that are pure logic and can
# be checked without a module attached.
#
#   sh test/native/run.sh
#
# telnet_filter.h is dependency free, so it compiles straight away. src/log.cpp
# is copied verbatim and built against a small Arduino shim, because the log
# ring is what a long burst of UART traffic quietly corrupts if it is wrong.
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp "$here/Arduino.h" "$here/config.h" "$here/test_log.cpp" "$work/"
cp "$root/src/log.cpp" "$work/log_under_test.cpp"
cp "$root/src/log.h"  "$work/log.h"
cp "$root/src/telnet_filter.h" "$work/"

if ! diff -q "$root/src/log.cpp" "$work/log_under_test.cpp" > /dev/null; then
    echo "src/log.cpp changed while copying, aborting" >&2
    exit 1
fi

echo "== telnet option negotiation"
c++ -std=c++17 -O1 -Wall -I"$work" "$here/test_telnet.cpp" -o "$work/test_telnet"
"$work/test_telnet"

echo
echo "== uart log ring"
c++ -std=c++17 -O1 -Wall -I"$work" "$work/test_log.cpp" "$work/log_under_test.cpp" \
    -o "$work/test_log"
"$work/test_log"
