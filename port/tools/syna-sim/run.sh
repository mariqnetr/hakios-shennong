#!/bin/sh
# The touch driver (xnu/files/bsd/dev/shennong_synaptics.c) against a simulated Synaptics S3910P on
# simulated pins: four cases, see syna_test.c. Needs only a C compiler.
set -e
cd "$(dirname "$0")"
out=${TMPDIR:-/tmp}/syna_test
cc -O1 -g -Wall -Wextra -I../../xnu/files/bsd/dev -o "$out" syna_test.c
for v in A B C D E; do "$out" $v | tail -1; done
