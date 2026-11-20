#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"
# where this phone's darwin is (DARWIN_ID=phones/<serial>.darwin; none: redwood's), made anew every build
python3 ../common/flash/darwinid.py header "${DARWIN_ID:+$(readlink -f "$DARWIN_ID")}" darwin_identity.h
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter ${PAYLOAD_BASE:+-DPAYLOAD_BASE=${PAYLOAD_BASE}ULL} \
  -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:loader.efi loader.obj
ls -la loader.efi
