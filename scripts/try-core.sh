#!/bin/bash
# With the phone in the loader (menu's USB (XL)): start the kernel on each core named, stopping at checkpoint 10, read only.
cd ~/tmp/shennong/build/hackios || exit 1
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
fastboot oem ramdisk flash 2>&1 | grep -E 'OKAY|FAILED' | cut -c1-80
for c in "$@"; do
	python3 xnuloader/mkpayload.py -o /tmp/try-payload.bin kernel=$K ramdisk=userland/root.apfs --cmdline "loader_return_after=20 epoch=$(date +%s) bootcpu=$c cpus=1" >/dev/null
	fastboot stage /tmp/try-payload.bin 2>&1 | grep -E 'OKAY|FAILED' | head -1 | cut -c1-60
	fastboot oem run 10 2>&1 | grep -E 'OKAY|FAILED' | head -1 | cut -c1-60
	t0=$(date +%s)
	while :; do
		r=$(timeout 5 fastboot getvar result 2>&1 | grep '^result:')
		case "$r" in *running*|"") ;; *) break ;; esac
		[ $(( $(date +%s) - t0 )) -gt 80 ] && break
		sleep 2
	done
	echo "core $c: $r after $(( $(date +%s) - t0 )) s"
done
