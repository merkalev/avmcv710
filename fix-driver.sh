#!/usr/bin/env bash
# fix-driver.sh - one-shot clean reload of the avmcv710 kernel driver.
#
# Why this exists: a registered AVMCV710 ALSA capture card pins its module,
# so a plain `rmmod avmcv710` fails with "Module avmcv710 is in use" even
# when nothing holds the device open.  The fixed build drops the card pin in
# disconnect(), so unbinding the USB interface first is enough to unload.
# The old (pre-fix) build does NOT do that; if it is still what is loaded on
# this host, the script now tells you the safe escape hatch instead of
# giving up silently.
#
# The replacement module registers no sound card by default (audio is opt-in,
# avm_audio=1), so this script is also the way to do a clean reload whenever
# audio was enabled:
#   sudo ./fix-driver.sh                    # video-only module (default)
#   sudo ./fix-driver.sh avm_audio=1        # with the ALSA capture card
#
# If the working tree has a build (./avmcv710.ko) whose srcversion differs from
# the installed module, this script installs the tree build first, so running
# it right after an edit always brings up the new code instead of silently
# reloading the previous one.
#
set -u

MOD="avmcv710"
DRV="/sys/bus/usb/drivers/$MOD"
EXPECTED_SRC="27DFE0F25007224D8224D56"   # 2026-10-16 overlays: scaled composite (macOS artwork)

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo ./fix-driver.sh)"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }

# --- 0. install the working tree's build if it is not what is loaded --------
# modprobe loads whatever lives under /lib/modules, NOT this directory.  Right
# after an edit + `make`, the tree has a fresh ./avmcv710.ko while the installed
# module is still the previous build, so reloading without installing silently
# brings back the OLD code (the classic "why didn't my fix take" trap).  Detect
# the mismatch and install the tree build first.
here="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$here/avmcv710.ko" ]; then
	built_src="$(modinfo "$here/avmcv710.ko" 2>/dev/null | awk '/^srcversion/{print $2}')"
	inst_src="$(modinfo "$MOD" 2>/dev/null | awk '/^srcversion/{print $2}')"
	if [ -n "$built_src" ] && [ "$built_src" != "$inst_src" ]; then
		echo "installed $MOD is ${inst_src:-none}; tree build is $built_src -> installing it"
		make -C "$here" KDIR="/lib/modules/$(uname -r)/build" LLVM=1 install >/dev/null \
			|| fail "make install failed"
		depmod "$(uname -r)"
		echo "installed fresh build ($built_src)"
	else
		echo "installed $MOD already matches the tree build (${inst_src:-none})"
	fi
else
	echo "note: no ./avmcv710.ko in $here; reloading the installed module as-is"
fi

# --- 1. close OBS first, unconditionally ---------------------------------
# OBS retries its capture source, so it can grab /dev/video0 in the gap
# between "check who holds it" and "unbind" - and an open fd keeps the video
# device alive, deferring the vdev release (and thus, on the OLD build, the
# module pin).  Kill it up front so unbind+rmmod have a stable window.
if pgrep -x obs >/dev/null; then
	echo "closing obs (it retries the capture source and can grab /dev/video0 mid-unbind)..."
	pkill -TERM -x obs || true; sleep 2
	pgrep -x obs >/dev/null && { echo "obs did not exit; force-killing"; pkill -KILL -x obs || true; sleep 1; }
fi

# --- 2. locate the bound CV710 interface (USB 07ca:0710, one interface) ----
intf=""
for i in /sys/bus/usb/devices/*/*:1.0; do
	[ -d "$i" ] || continue
	case "$(readlink -f "$i/driver" 2>/dev/null || true)" in
		*"$MOD") intf="${i##*/}"; break ;;
	esac
done
[ -n "$intf" ] && echo "found interface $intf bound to $MOD" || echo "note: no interface is bound to $MOD today"

# --- 3. unbind -> disconnect drops the ALSA card pin (fixed build) ---------
if [ -n "$intf" ] && [ -w "$DRV/unbind" ]; then
	echo "unbinding $intf (disconnect frees the AVMCV710 sound card)..."
	echo "$intf" > "$DRV/unbind" || fail "unbind of $intf failed"
	sleep 2
fi

# --- 4. unload the old module ----------------------------------------------
# With the fixed build the pin is gone as soon as disconnect runs; with the
# old build a stray ref can linger invisibly (no open fds, no card node) and
# plain rmmod keeps failing.  Show the refcount each try so the drift is
# visible, and if it is stuck, hand the user the deliberate force-unload.
echo "unloading $MOD..."
ref_prev=""
for n in $(seq 1 20); do
	lsmod | grep -q "^$MOD " || { echo "module already gone"; break; }
	ref="$(cat /sys/module/$MOD/refcnt 2>/dev/null || echo '?')"
	[ "$ref" != "$ref_prev" ] && echo "  refcnt=$ref (attempt $n/20)"
	ref_prev="$ref"
	rmmod "$MOD" 2>/dev/null && { echo "rmmod ok"; break; }
	sleep 1
done

if lsmod | grep -q "^$MOD "; then
	ref="$(cat /sys/module/$MOD/refcnt 2>/dev/null || echo '?')"
	echo "---"
	echo "rmmod still fails: module avmcv710 is loaded with refcnt=$ref."
	echo "Video node and sound card are already gone; the remaining ref is"
	echo "an internal leak of the OLD loaded build.  Verify nothing holds the"
	echo "device, then force-unload DELIBERATELY and load the fixed build:"
	echo
	echo "  sudo rmmod -f avmcv710 && sudo modprobe avmcv710 $*"
	echo
	echo "Do NOT force-unload while OBS/PipeWire are running or while a"
	echo "capture is in progress."
	exit 1
fi

# --- 5. load the freshly installed module (audio off unless asked) ---------
kver="$(uname -r)"
if [ -f "./avmcv710.ko.zst" ]; then
	if [ ! -f "/lib/modules/$kver/updates/avmcv710.ko.zst" ] || [ "./avmcv710.ko.zst" -nt "/lib/modules/$kver/updates/avmcv710.ko.zst" ]; then
		echo "installing ./avmcv710.ko.zst to /lib/modules/$kver/updates/..."
		mkdir -p "/lib/modules/$kver/updates"
		cp -f "./avmcv710.ko.zst" "/lib/modules/$kver/updates/"
		depmod "$kver"
	fi
fi

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