#!/bin/sh
# Tests phosphor.cpp on a PC: Main's functions are replaced by stand-ins (stubs.cpp).
set -e
cd "$(dirname "$0")"
g++ -fsanitize=address,undefined -g -I.. test_api.cpp stubs.cpp ../phosphor.cpp -o /tmp/phosphor_api_test
PHOSPHOR_MAIN_SOCK=/tmp/phosphor_api_test.sock /tmp/phosphor_api_test | grep -v '^phosphor:'
