#!/bin/bash
# Host test of the ESP32 <-> S32K link protocol. Needs any C99 compiler; on Windows
# `python -m pip install ziglang` and CC="python -m ziglang cc" works.
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
$CC -std=c99 -Wall -Wextra -Werror -O2 -o be_link_test be_link_test.c ../../Software/src/communication/link/be_link.c
./be_link_test
