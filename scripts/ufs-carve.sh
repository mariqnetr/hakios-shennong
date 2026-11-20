#!/bin/bash
# Cut darwin (64 GiB, APFS) off the end of userdata on shennong, wipe Android's data, put the iOS system
# image into darwin. NOT REVERSIBLE for Android's data. Runs only with CARVE=yes.
#
#   cd ~/tmp/shennong && CARVE=yes bash ufs-carve.sh
#
# The tables were made by tools/userdata-carve.py --wipe from what ufs-layout.sh read (ufs-layout/):
#   userdata (sda33): LBA 2260480..64049144 -> 2260480..47271423 (171.70 GiB)
#   darwin   (sda34): LBA 47271424..64049144 (64.00 GiB), unique GUID 6fbd0b67-9d2b-446a-854a-d0d8cde63ba0
# Nothing else in the table changes. What this does, stopping at the first thing that is not as expected:
#   1. Android with root, the phone is shennong, userdata is sda33
#   2. the phone's GPT of LUN 0 (sda) now is byte for byte the one read by ufs-layout.sh
#   3. the new GPT pushed, its sha256 checked on the phone; written (each copy: table, then header),
#      read back and compared
#   4. adb reboot bootloader; fastboot -w (userdata and metadata erased, Android makes them anew)
#   5. fastboot flash darwin userland/system.apfs (the image `xl ramdisk flash` reads), fastboot reboot
# A copy of the old tables stays in ufs-layout/gpt-sda-{head,tail}.bin: putting them back the same way
# (dd at LBA 1..3 and 64049146..64049151) gives userdata its old size (then fastboot -w again).
set -e
cd "$(dirname "$0")"
L=ufs-layout
IMG=build/hackios/userland/system.apfs
DISK=64049152
[ "$CARVE" = yes ] || { echo "ufs-carve: needs CARVE=yes (erases Android's data)"; exit 1; }
for f in $L/gpt-sda-head.bin $L/gpt-sda-tail.bin $L/gpt-sda-head.new $L/gpt-sda-tail.new $IMG; do
	[ -f $f ] || { echo "ufs-carve: $f missing"; exit 1; }
done
sha() { shasum -a 256 "$1" | cut -c1-64; }
su() { adb exec-out su -c "$1"; }

echo "== 1. the phone"
[ "$(adb get-state 2>/dev/null)" = device ] || { echo "ufs-carve: no phone in Android on adb"; exit 1; }
[ "$(su id | grep -c 'uid=0')" = 1 ] || { echo "ufs-carve: no root shell (KernelSU: allow shell)"; exit 1; }
[ "$(adb shell getprop ro.product.device | tr -d '\r')" = shennong ] || { echo "ufs-carve: not shennong"; exit 1; }
[ "$(su 'readlink /dev/block/by-name/userdata' | tr -d '\r')" = /dev/block/sda33 ] || { echo "ufs-carve: userdata is not sda33"; exit 1; }
[ "$(su 'blockdev --getsize64 /dev/block/sda' | tr -d '\r')" = $((DISK * 4096)) ] || { echo "ufs-carve: sda is not the size it was"; exit 1; }

echo "== 2. the table on the phone is the one the new one was made from"
T=$(mktemp -d)
su "dd if=/dev/block/sda bs=4096 count=8 2>/dev/null" > $T/head
su "dd if=/dev/block/sda bs=4096 skip=$((DISK - 8)) count=8 2>/dev/null" > $T/tail
[ "$(sha $T/head)" = "$(sha $L/gpt-sda-head.bin)" ] && [ "$(sha $T/tail)" = "$(sha $L/gpt-sda-tail.bin)" ] ||
	{ echo "ufs-carve: the phone's GPT changed since ufs-layout.sh: nothing written"; exit 1; }

echo "== 3. the new table"
adb push $L/gpt-sda-head.new /data/local/tmp/gpt-head.new >/dev/null
adb push $L/gpt-sda-tail.new /data/local/tmp/gpt-tail.new >/dev/null
[ "$(su 'sha256sum /data/local/tmp/gpt-head.new' | cut -c1-64)" = "$(sha $L/gpt-sda-head.new)" ] &&
[ "$(su 'sha256sum /data/local/tmp/gpt-tail.new' | cut -c1-64)" = "$(sha $L/gpt-sda-tail.new)" ] ||
	{ echo "ufs-carve: the copies on the phone are not the files: nothing written"; exit 1; }
su "cd /data/local/tmp &&
    dd if=gpt-head.new of=/dev/block/sda bs=4096 skip=2 seek=2 count=2 conv=notrunc,fsync &&
    dd if=gpt-head.new of=/dev/block/sda bs=4096 skip=1 seek=1 count=1 conv=notrunc,fsync &&
    dd if=gpt-tail.new of=/dev/block/sda bs=4096 skip=2 seek=$((DISK - 6)) count=6 conv=notrunc,fsync && sync" 2>&1 | grep -v records || true
su "dd if=/dev/block/sda bs=4096 count=8 2>/dev/null" > $T/head
su "dd if=/dev/block/sda bs=4096 skip=$((DISK - 8)) count=8 2>/dev/null" > $T/tail
if [ "$(sha $T/head)" != "$(sha $L/gpt-sda-head.new)" ] || [ "$(sha $T/tail)" != "$(sha $L/gpt-sda-tail.new)" ]; then
	echo "ufs-carve: READ BACK DIFFERS -- do not reboot; tell Claude (the files are in $T)"; exit 1
fi
echo "   written and read back the same"

echo "== 4. bootloader, wipe"
adb reboot bootloader
for i in $(seq 60); do fastboot devices | grep -q fastboot && break; sleep 2; done
fastboot getvar partition-size:darwin 2>&1 | grep -i darwin
fastboot -w

echo "== 5. the system image into darwin"
fastboot flash darwin $IMG
fastboot reboot
echo "ufs-carve: done. Android starts anew (setup screen); darwin holds $(($(stat -f %z $IMG) >> 20)) MB of system.apfs"
