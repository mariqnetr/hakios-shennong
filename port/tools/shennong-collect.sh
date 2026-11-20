#!/bin/bash
# Снять с Xiaomi 14 Pro (shennong) всё, что нужно для порта HackiOS. Запускать на компьютере,
# к которому телефон подключён по USB с включённой отладкой. Ничего на телефон не пишет.
#
# usage: tools/shennong-collect.sh [папка]   (по умолчанию ./shennong-data)
set -u
OUT=${1:-shennong-data}
mkdir -p "$OUT" && cd "$OUT" || exit 1
adb get-state >/dev/null 2>&1 || { echo "нет телефона в adb (adb devices)"; exit 1; }

su_cat() { adb exec-out su -c "cat $1" 2>/dev/null; }
have_root=0; [ "$(adb shell su -c id 2>/dev/null | grep -c uid=0)" = 1 ] && have_root=1
echo "root: $have_root"

adb shell getprop > getprop.txt
adb shell cat /proc/cpuinfo > cpuinfo.txt
adb shell uname -a > uname.txt
adb shell getevent -lp > getevent.txt 2>&1          # тачскрин: имя устройства, диапазоны осей
adb shell dumpsys SurfaceFlinger --display-id > display.txt 2>&1
adb shell wm size >> display.txt 2>&1
adb shell 'ls -la /vendor/firmware /vendor/firmware_mnt/image /vendor/lib/modules' > fwlist.txt 2>&1
adb shell lsmod > lsmod.txt 2>&1 || adb shell cat /proc/modules > lsmod.txt 2>&1

# device tree: без root обычно не читается. exec-out отдаёт и текст ошибки, поэтому проверяем
# магию FDT (d00dfeed), а не размер файла
is_fdt() { [ "$(head -c 4 "$1" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = d00dfeed ]; }
adb exec-out cat /sys/firmware/fdt > fdt.dtb 2>/dev/null
is_fdt fdt.dtb || { [ $have_root = 1 ] && su_cat /sys/firmware/fdt > fdt.dtb; }
is_fdt fdt.dtb || rm -f fdt.dtb

if [ $have_root = 1 ]; then
	su_cat /proc/iomem > iomem.txt
	adb exec-out su -c dmesg > dmesg.txt 2>/dev/null
	# какой драйвер у тача и на какой шине
	adb exec-out su -c 'for d in /sys/class/input/input*; do echo "$d: $(cat $d/name) -> $(readlink -f $d/device)"; done' > input-drivers.txt 2>&1
	# разделы прошивки для UEFIReader (xbl, uefi) и device tree (vendor_boot), только чтение
	SLOT=$(adb shell getprop ro.boot.slot_suffix | tr -d '\r')
	mkdir -p parts
	for p in xbl xbl_config uefi uefisecapp vendor_boot dtbo; do
		adb exec-out su -c "[ -e /dev/block/by-name/$p$SLOT ] && cat /dev/block/by-name/$p$SLOT" > parts/$p.img
		[ -s parts/$p.img ] || rm -f parts/$p.img
	done
fi

echo; ls -la
[ -f fdt.dtb ] && echo "fdt.dtb есть: пропиши его в DTB= в common/devices/shennong.conf и сверь GIC/ядра/память (docs/PORTING-shennong.md)" \
	|| echo "fdt.dtb нет (нужен root или dtb из vendor_boot.img прошивки)"
