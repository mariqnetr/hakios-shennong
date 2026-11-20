#!/bin/bash
# build.sh PROGRAM.c [OUT]: a host test against src/
O=/opt/homebrew/opt/openssl
cd "$(dirname "$0")"
clang -O1 -g -Wall -Wno-unused-function -I src -I $O/include "$1" hostdefl.c src/apfs_fsw.c src/apfs_vw.c src/apfs_bt.c \
  src/apfs_space.c src/apfs_txn.c src/apfs_lib.c src/libdeflate/lib/*.c src/libzbitmap/libzbitmap.c \
  -L $O/lib -lcrypto -lz -o "${2:-${1%.c}}" 2>&1 | grep -E 'error|warning: .*apfs_(fsw|vw|bt|space|txn)' || true
