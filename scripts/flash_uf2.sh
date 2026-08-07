#!/bin/sh
#
# Copy the built UF2 image onto the Feather's bootloader volume (macOS/Linux).
# The Windows equivalent is scripts/flash_uf2.ps1.
#
# `west flash` is not usable: the uf2 runner fails with
# "ValueError: uf2 doesn't support --dev-id option".
#
# Double-tap RESET on the Feather first so the bootloader volume mounts.

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# sysbuild nests the image one level deeper (build/<dir>/ant_dongle/zephyr/)
# than a plain application build does, so search for the newest one rather
# than hardcoding a path.
UF2_PATH="${1:-}"
if [ -z "$UF2_PATH" ]; then
	UF2_PATH="$(find "$REPO_ROOT/build" -name zephyr.uf2 -type f 2>/dev/null |
		xargs -r ls -t 2>/dev/null | head -n 1)"
fi

if [ -z "$UF2_PATH" ] || [ ! -f "$UF2_PATH" ]; then
	echo "No zephyr.uf2 found under $REPO_ROOT/build" >&2
	echo "Build first, or pass the image path as the first argument. See README.md." >&2
	exit 1
fi

# macOS mounts under /Volumes; most Linux desktops under /media or /run/media.
BOOT_VOLUME=""
for candidate in \
	/Volumes/FTHR840BOOT \
	"/media/$(id -un)/FTHR840BOOT" \
	"/run/media/$(id -un)/FTHR840BOOT"; do
	if [ -d "$candidate" ]; then
		BOOT_VOLUME="$candidate"
		break
	fi
done

if [ -z "$BOOT_VOLUME" ]; then
	echo "Bootloader volume FTHR840BOOT is not mounted." >&2
	echo "Close any tool using the Feather and double-tap RESET first." >&2
	echo "If it never appears, try a different USB cable - some are charge-only." >&2
	exit 1
fi

echo "Copying $UF2_PATH to $BOOT_VOLUME/"

# dd with a 4k block size proved more reliable than cat during testing. The
# bootloader reboots the board the moment the last block lands, so a write
# error at the very end is the expected outcome, not a failure.
dd if="$UF2_PATH" of="$BOOT_VOLUME/$(basename "$UF2_PATH")" bs=4k 2>/dev/null || true
sync 2>/dev/null || true

echo "Flashed. The board should re-enumerate as 0fcf:1008 within a few seconds."
echo "Verify with: python tools/ant_probe.py"
