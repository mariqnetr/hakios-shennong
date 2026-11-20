#!/bin/bash
# The image of an installed phone: Mu-Silicium UEFI (RELEASE), the loader with its boot menu, the kernel and root.apfs of
# this tree, for shennong's boot_b. Nothing is written to the phone:
#
#   cd ~/tmp/shennong && bash build-install.sh           -> build/out/shennong-install.img
#
#   fastboot boot build/out/shennong-install.img         first: in RAM only, Android's slot stays active, nothing is written
#   fastboot flash boot_b build/out/shennong-install.img
#   fastboot set_active b && fastboot reboot             the menu at every start;  back: fastboot set_active a
#
# The kernel's command line is INSTALL_CMDLINE (no time limit: 1 year; the epoch is the phone's own clock when it has one).
# Needs: build-kernel.sh run before, build/out/SILICIUM_UEFI-RELEASE.fd (build/inner-uefi-release.sh); the loader from LOADER.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
HK=${HK:-$HOME/tmp/apfsk/hk}
OUT=$HERE/build/out
FD=${FD:-$OUT/SILICIUM_UEFI-RELEASE.fd}
LOADER=${LOADER:-$HK/xnuloader/loader.efi}
K=$HERE/build/hackios/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
RD=${RD:-$HERE/build/hackios/userland/root.apfs}
CMD=${INSTALL_CMDLINE:-fb_phone=2796 syna_touch=1 cpufreq=1 bootcpu=7 cpus=8 fb_panel_hz=120 fb_hz=120 fb_threads=4 battmgr=1 wifi=1 wifi_ba=1 wifi_wide=1 wlan=13 wlan_driver=1 wlan_pause=0 wifi_station=1 wlan_fw=amss_lf.bin wlan_m3=m3_lf.bin wlan_board2=board2_lf.bin bt=4 ufs=2 ufs_write=1 ufs_bwrite=1 darwin_lba=47271424 darwin_blocks=16777721 var_apfs=3145728 haptic=4 haptic_mv=2000}
for f in "$FD" "$LOADER" "$K" "$RD"; do [ -f "$f" ] || { echo "build-install: $f is not there"; exit 1; }; done
mkdir -p "$HERE/build/install"
cp "$LOADER" "$HERE/build/install/loader.efi"
cp "$HK/xnuloader/mkpayload.py" "$HERE/build/install/mkpayload.py"
cp "$K" "$HERE/build/install/kernel"
cp "$RD" "$HERE/build/install/root.apfs"
cat > "$HERE/build/install/make.sh" <<INNER
#!/bin/bash
set -e
cd /work/hackios
python3 /work/install/mkpayload.py -o /work/install/payload.bin --menu loader.efi=/work/install/loader.efi kernel=/work/install/kernel ramdisk=/work/install/root.apfs \
	--cmdline "loader_return_after=31536000 epoch=$(date +%s) sb_delay=8 $CMD"
set -a; . common/devices/shennong.conf; set +a
UEFI_FD=/work/out/$(basename $FD) UEFI_TARGET=shennong FD_BASE=\$FD_BASE FD_SIZE=\$FD_SIZE PAYLOAD_BASE=\$PAYLOAD_BASE DTB= \
	python3 mkxnuimg.py -p /work/install/payload.bin -o /work/out/shennong-install.img
INNER
podman run --rm --platform linux/amd64 -v "$HERE/build:/work" -v shennong-edk2-build:/work/hackios/Mu-Silicium/Build shennong-build bash /work/install/make.sh
python3 - "$HERE/build/install/payload.bin" <<'PY'
import struct,sys
d=open(sys.argv[1],"rb").read();p=8
print("payload sums")
while p+24<=len(d):
    n=d[p:p+16].split(b"\0")[0]
    if not n: break
    z=struct.unpack("<I",d[p+16:p+20])[0]
    print(n.decode(),z,hex(sum(d[p+24:p+24+z])&0xffffffff))
    p+=24+((z+15)&~15)
PY
ls -la "$OUT/shennong-install.img"; shasum -a 256 "$OUT/shennong-install.img"
