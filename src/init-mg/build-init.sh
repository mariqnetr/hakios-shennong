#!/bin/bash
# build init (userland/src/init.c) with libmini, as userland/build.sh does, in the kernel build container
set -e
cd /k/hackios && source ./xnu-env.sh >/dev/null 2>&1 || true
export PATH=/k/hackios/xnu-deps/host/bin:/k/theos/toolchain/linux/iphone/bin:$PATH
cd /out
CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
$CC -Ilibmini -c libmini/mini.c -o mini.o
$CC -c libmini/start.S -o start.o
$CC -Ilibmini $LD -o init.bin init.c mini.o start.o
ls -la init.bin
