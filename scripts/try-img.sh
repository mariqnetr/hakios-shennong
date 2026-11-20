#!/bin/bash
# try-img.sh IMG: boot IMG from fastboot and wait up to 3 min for the HackiOS loader
cd "$(dirname "$0")"
fastboot boot "$1" || exit 1
for i in $(seq 60); do
	sleep 3
	p=$(fastboot getvar product 2>&1 | grep '^product:')
	echo "$((i*3))s ${p:-absent}"
	case "$p" in *xnuloader*) hackios/xl log | grep 'usb:'; echo "LOADER UP"; exit 0 ;; esac
done
echo "no loader"; exit 1
