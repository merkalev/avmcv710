#!/usr/bin/env bash
# fix-driver.sh - one-shot clean reload of the avmcv710 kernel driver.
#
# Why this exists: the old module registered the AVMCV710 ALSA capture card,
# and a registered snd card pins its module, so a plain `rmmod avmcv710`
# fails with "Module avmcv710 is in use" even with no program holding the
# device open.  This script unbinds the USB interface first (which runs the
# driver's disconnect -> frees the ALSA card -> drops the pin), then unloads
# and loads the freshly installed module.
#
# The replacement module registers no sound card by default (audio is opt-in,
# avm_audio=1), so this script is also the way to do a clean reload whenever
# audio was enabled:
#   sudo ./fix-driver.sh                    # video-only module (default)
#   sudo ./fix-driver.sh avm_audio=1        # with the ALSA capture card
#
set -u

MOD="avmcv710"
DRV="/sys/bus/usb/drivers/$MOD"
EXPECTED_SRC="F8A0D1FB8A2BD916237B40C"   # module with the ops->prepare fix

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo ./fix-driver.sh)"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }

# --- 1. locate the bound CV710 interface (USB 07ca:0710, one interface) ----
intf=""
for i in /sys/bus/usb/devices/*/*:1.0; do
	[ -d "$i" ] || continue
	case "$(readlink -f "$i/driver" 2>/dev/null || true)" in
		*"$MOD") intf="${i##*/}"; break ;;
	esac
done
[ -n "$intf" ] && echo "found interface $intf bound to $MOD" || echo "note: no interface is bound to $MOD today"

# --- 2. close whatever holds /dev/video0 (OBS retries its source) ----------
if fuser /dev/video0 >/dev/null 2>&1; then
	echo "--- /dev/video0 is held by:"
	fuser -v /dev/video0 2>&1 || true
	if pgrep -x obs >/dev/null; then
		echo "closing obs (it owns the capture device)..."
		pkill -TERM -x obs || true; sleep 2
		pgrep -x obs >/dev/null && { echo "obs did not exit; force-killing"; pkill -KILL -x obs || true; sleep 1; }
	else
		fail "a process other than obs holds /dev/video0 - close it and re-run"
	fi
fi

# --- 3. unbind -> disconnect frees the ALSA card (drops the module pin) ----
if [ -n "$intf" ] && [ -w "$DRV/unbind" ]; then
	echo "unbinding $intf (disconnect frees the AVMCV710 sound card)..."
	echo "$intf" > "$DRV/unbind" || fail "unbind of $intf failed"
	sleep 1
fi

# --- 4. unload the old module (retry briefly for stray refs) ---------------
echo "unloading $MOD..."
for _ in 1 2 3 4 5 6 7 8 9 10; do
	lsmod | grep -q "^$MOD " || break
	rmmod "$MOD" 2>/dev/null && break
	sleep 1
done
lsmod | grep -q "^$MOD " && fail "rmmod $MOD still fails (module in use) - see fuser output above"

# --- 5. load the freshly installed module (audio off unless asked) ---------
echo "loading $MOD $* ..."
modprobe "$MOD" "$@" || fail "modprobe $MOD $* failed"
sleep 1

# --- 6. verify ---------------------------------------------------------------
echo "--- verification ---"
if cat /proc/asound/cards | grep -q AVMCV710; then
	echo "WARN: AVMCV710 sound card still registered!"
else
	echo "OK: no AVMCV710 sound card registered"
fi
[ -e /dev/video0 ] && echo "OK: /dev/video0 present" || echo "WARN: /dev/video0 missing"
src="$(modinfo "$MOD" 2>/dev/null | grep -m1 '^srcversion' | awk '{print $2}')"
echo "module srcversion: ${src:-n/a} (expected $EXPECTED_SRC)"
[ "$src" = "$EXPECTED_SRC" ] || echo "WARN: loaded module is not the fixed build"
lsmod | grep "^$MOD "
echo "--- recent avmcv710 log lines:"
journalctl -k -b --no-pager 2>/dev/null | grep -E "avmcv710" | tail -3
echo "done."