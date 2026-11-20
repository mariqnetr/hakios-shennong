#!/bin/bash
# Снять разметку UFS телефона из Android (adb, root через KernelSU) в ufs-layout/. Только чтение:
# на телефоне ничего не пишется, кроме вывода команд в pipe adb.
#
#   cd ~/tmp/shennong && bash ufs-layout.sh
#
# Что сохраняется:
#   by-name.txt          имя раздела -> sdXN
#   partitions.txt       /proc/partitions
#   sysfs.txt            для каждого sdXN: номер, имя, start, size (в секторах по 512)
#   luns.txt             логические юниты (sda..sdf): размер блока, число блоков
#   gpt-sdX-head.bin     первые 8 блоков по 4096 каждого юнита (GPT: заголовок и таблица)
#   gpt-sdX-tail.bin     последние 8 блоков (резервная копия GPT)
#   userdata.txt         df /data, mount, f2fs segment_info и status, dm-таблица userdata
#   userdata-sb.bin      первый блок userdata (суперблок f2fs; при metadata encryption -- шифротекст)
#   props.txt            слот, шифрование, модель
set -e
cd "$(dirname "$0")"
OUT=ufs-layout
mkdir -p $OUT
[ "$(adb get-state 2>/dev/null)" = device ] || { echo "нужен телефон в Android с adb"; exit 1; }
su() { adb exec-out su -c "$1"; }

su 'ls -l /dev/block/by-name/' > $OUT/by-name.txt
su 'cat /proc/partitions' > $OUT/partitions.txt
su 'for d in /sys/class/block/sd*[0-9]; do n=${d##*/}; echo "$n partn=$(cat $d/partition) name=$(grep PARTNAME $d/uevent | cut -d= -f2) start=$(cat $d/start) size=$(cat $d/size)"; done' > $OUT/sysfs.txt
su 'for d in /sys/class/block/sd[a-z]; do n=${d##*/}; echo "$n lbs=$(cat $d/queue/logical_block_size) sectors=$(cat $d/size) $(cat $d/device/scsi_device/*/device/vendor 2>/dev/null) $(readlink $d/device)"; done' > $OUT/luns.txt
for d in $(awk '{print $1}' $OUT/luns.txt); do
	su "dd if=/dev/block/$d bs=4096 count=8 2>/dev/null" > $OUT/gpt-$d-head.bin
	blocks=$(su "blockdev --getsize64 /dev/block/$d" | tr -d '\r')
	su "dd if=/dev/block/$d bs=4096 skip=$((blocks / 4096 - 8)) count=8 2>/dev/null" > $OUT/gpt-$d-tail.bin
done
{
	su 'df /data; echo; mount | grep -E " /data |/metadata "; echo'
	su 'ls -l /dev/block/by-name/userdata; dmsetup table 2>/dev/null; echo'
	su 'for f in /proc/fs/f2fs/*/; do echo "== $f"; cat $f/segment_info 2>/dev/null | head -5; done; echo'
	su 'cat /sys/fs/f2fs/*/features 2>/dev/null; ls /sys/fs/f2fs/'
} > $OUT/userdata.txt 2>&1
su 'dd if=/dev/block/by-name/userdata bs=4096 count=1 2>/dev/null' > $OUT/userdata-sb.bin
adb shell getprop | grep -E 'slot_suffix|crypto|product.device|product.model|ufs|storage' > $OUT/props.txt
ls -l $OUT
echo "готово: $OUT/ (на телефоне ничего не записано)"
