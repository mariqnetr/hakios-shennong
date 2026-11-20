#!/system/bin/sh
# Run on the phone in Android, as root (nothing is written). Before making slot b active: are the partitions the
# bootloaders start from the same in both slots? Slot b's firmware runs when b is active.
#   adb push slot-check.sh /data/local/tmp/ && adb shell su -c 'sh /data/local/tmp/slot-check.sh'
cd /dev/block/by-name || exit 1
for a in *_a; do
	n=${a%_a}; b=${n}_b
	[ -e "$b" ] || continue
	sa=$(sha256sum "$a" | cut -c1-16); sb=$(sha256sum "$b" | cut -c1-16)
	if [ "$sa" = "$sb" ]; then echo "same    $n $sa"; else echo "DIFFER  $n $sa $sb"; fi
done
getprop ro.boot.slot_suffix
getprop ro.build.fingerprint
bootctl get-current-slot 2>&1
bootctl get-suffix 1 2>&1
bootctl is-slot-bootable 1 2>&1
bootctl is-slot-marked-successful 1 2>&1
command -v bootctl
