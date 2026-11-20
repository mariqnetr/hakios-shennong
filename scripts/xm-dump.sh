#!/system/bin/sh
# Run on the phone as root, on the 120 W charger: dumps what Android knows of the battery and the charger.
for d in /sys/class/qcom-battery /sys/class/xm_power /sys/class/xm-battery /sys/class/power_supply/*; do
	[ -d "$d" ] || continue
	echo "### $d"
	for f in "$d"/*; do
		[ -f "$f" ] || continue
		v=$(cat "$f" 2>/dev/null | head -c 200 | tr '\n' ' ')
		echo "$(basename "$f")=$v"
	done
done
echo "### classes"
ls /sys/class | grep -i -E 'batt|power|xm|qcom|charg|usbpd'
echo "### dmesg"
dmesg | grep -i -E 'BATTERY_CHG|verif|uvdm|authen|pd_|apdo|power_max|quick' | tail -300
