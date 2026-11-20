#!/bin/bash
# Keeps what a run started from the menu says: asks the loader for its log every 2 s while a kernel runs. Read only.
D=$HOME/tmp/shennong/live; rm -rf "$D"; mkdir -p "$D"; t0=$(date +%s); n=0; ran=0
while [ $(( $(date +%s) - t0 )) -lt ${1:-420} ]; do
	if timeout 3 fastboot devices 2>/dev/null | grep -q '^xnuloader'; then
		r=$(timeout 5 fastboot getvar result 2>&1 | grep '^result:')
		case "$r" in *running*) ran=1 ;; esac
		if [ $ran = 1 ]; then
			timeout 8 fastboot oem log >/dev/null 2>&1
			n=$((n+1)); timeout 15 fastboot get_staged "$D/$n.log" >/dev/null 2>&1
			[ -s "$D/$n.log" ] && tr -d '\0' < "$D/$n.log" > "$D/last.log"
			echo "$(date +%T) $n $r $(wc -c < "$D/last.log" 2>/dev/null)" >> "$D/index.txt"
		fi
	elif [ $ran = 1 ]; then echo "$(date +%T) gone" >> "$D/index.txt"; sleep 20; [ -s "$D/last.log" ] && timeout 3 fastboot devices | grep -q . || true; break
	fi
	sleep 2
done
echo done >> "$D/index.txt"
