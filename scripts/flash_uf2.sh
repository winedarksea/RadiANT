#!/bin/sh
#
# Copy the built UF2 image onto an Adafruit-style UF2 bootloader volume
# (macOS/Linux): the Feather nRF52840 (FTHR840BOOT) or a Pro Micro nRF52840 /
# nice!nano / SuperMini clone (NICENANO, or NRF52BOOT on some clones). The
# Windows equivalent is scripts/flash_uf2.ps1.
#
# `west flash` is not usable: the uf2 runner fails with
# "ValueError: uf2 doesn't support --dev-id option".
#
# Double-tap RESET first so the bootloader volume mounts. A Pro Micro with no
# button needs RST bridged to GND twice in quick succession.
#
# Usage: scripts/flash_uf2.sh [path/to/zephyr.uf2]
#   UF2_VOLUME=<label or mount path>   pick the volume explicitly; the same
#                                      job as flash_uf2.ps1's -VolumeLabel

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# sysbuild nests the image one level deeper (build/<dir>/radiant_dongle/zephyr/)
# than a plain application build does, so search for the newest one rather
# than hardcoding a path. `-exec ... {} +` rather than `| xargs`: xargs splits
# on whitespace, so a checkout under a path with a space found nothing.
UF2_PATH="${1:-}"
if [ -z "$UF2_PATH" ]; then
	UF2_PATH="$(find "$REPO_ROOT/build" -name zephyr.uf2 -type f \
		-exec ls -t {} + 2>/dev/null | head -n 1)"
fi

if [ -z "$UF2_PATH" ] || [ ! -f "$UF2_PATH" ]; then
	echo "No zephyr.uf2 found under $REPO_ROOT/build" >&2
	echo "Build first, or pass the image path as the first argument. See README.md." >&2
	exit 1
fi

# macOS mounts under /Volumes; most Linux desktops under /media or /run/media.
USER_NAME="$(id -un)"
LABELS="FTHR840BOOT NICENANO NRF52BOOT"
BOOT_VOLUME=""
case "${UF2_VOLUME:-}" in
	"") ;;
	*/*) LABELS="" ; [ -d "$UF2_VOLUME" ] && BOOT_VOLUME="$UF2_VOLUME" ;;
	*) LABELS="$UF2_VOLUME" ;;
esac
for label in $LABELS; do
	for candidate in \
		"/Volumes/$label" \
		"/media/$USER_NAME/$label" \
		"/run/media/$USER_NAME/$label"; do
		if [ -d "$candidate" ]; then
			BOOT_VOLUME="$candidate"
			break 2
		fi
	done
done

if [ -z "$BOOT_VOLUME" ]; then
	echo "No UF2 bootloader volume is mounted (looked for: ${UF2_VOLUME:-$LABELS})." >&2
	echo "Close any tool using the board and double-tap RESET first." >&2
	echo "A different label can be given with UF2_VOLUME=<label or path>." >&2
	echo "If it never appears, try a different USB cable - some are charge-only." >&2
	exit 1
fi

echo "Copying $UF2_PATH to $BOOT_VOLUME/"

# dd with a 4k block size proved more reliable than cat during testing. The
# bootloader reboots the board the moment the last block lands, so a write
# error at the very end is the expected outcome, not a failure.
dd if="$UF2_PATH" of="$BOOT_VOLUME/$(basename "$UF2_PATH")" bs=4k 2>/dev/null || true
sync 2>/dev/null || true

echo "Flashed. The board should re-enumerate as 0fcf:1009 within a few seconds."
echo "Verify with: python tools/ant_probe.py"
