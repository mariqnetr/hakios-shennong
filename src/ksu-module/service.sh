#!/system/bin/sh
# After Android has come up and marked its slot good: slot b active again, through the boot control service,
# which changes the active bits, the type GUIDs and the boot LUN together. The menu's "Android" brings the phone back to a.
# Log: /data/adb/hackios-menu.log. Nothing happens unless everything below is as expected.
SVC=android.hardware.boot.IBootControl/default
LOG=/data/adb/hackios-menu.log
say() { [ -n "$DRY_RUN" ] && { echo "$*"; return; }; echo "$(date '+%F %T') $*" >> "$LOG"; }
# the transactions of IBootControl (AIDL, in the order of its file)
GET_ACTIVE=1 GET_CURRENT=2 GET_SLOTS=3 GET_MERGE=4 IS_SUCCESSFUL=7 SET_ACTIVE=9
ask() { # ask CODE [SLOT]: the int the service answers; empty when it says anything else
	local out val
	out=$(service call "$SVC" "$1" ${2:+i32 "$2"} 2>/dev/null | tr -s '\t ' ' ')
	val=$(echo "$out" | sed -n "s/.*Parcel( *00000000 \([0-9a-f]\{8\}\).*/\1/p")
	[ -n "$val" ] && printf '%d\n' "0x$val"
}
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 5; done
# update_verifier marks the slot; wait for it, five minutes at most
n=0
until [ "$(ask $IS_SUCCESSFUL 0)" = 1 ]; do
	n=$((n + 1)); [ $n -gt 30 ] && { say "slot a not marked successful after 5 minutes: nothing changed"; exit 0; }
	sleep 10
done
[ "$(ask $GET_SLOTS)" = 2 ] || { say "not two slots: nothing changed"; exit 0; }
[ "$(ask $GET_CURRENT)" = 0 ] || { say "not running from slot a: nothing changed"; exit 0; }
[ "$(ask $GET_MERGE)" = 0 ] || { say "a snapshot merge is pending or its state unknown: nothing changed"; exit 0; }
[ "$(ask $GET_ACTIVE)" = 0 ] || { say "slot a is not the active one: nothing to do"; exit 0; }
[ -n "$DRY_RUN" ] && { echo "all checks passed: would make slot b active"; exit 0; }
service call "$SVC" $SET_ACTIVE i32 1 >/dev/null 2>&1
if [ "$(ask $GET_ACTIVE)" = 1 ]; then say "slot b active: the menu at the next start"; else say "slot b did NOT become active"; fi
