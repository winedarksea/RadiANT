#!/bin/sh

set -eu

UF2_PATH="build/ant_dongle/zephyr/zephyr.uf2"
BOOT_VOLUME="/Volumes/FTHR840BOOT"

if [ ! -f "$UF2_PATH" ]; then
	echo "UF2 image not found at $UF2_PATH" >&2
	echo "Build first with west -z /opt/nordic/ncs/v3.2.4/zephyr build -b adafruit_feather_nrf52840/nrf52840/uf2 -p always" >&2
	exit 1
fi

if [ ! -d "$BOOT_VOLUME" ]; then
	echo "Bootloader volume $BOOT_VOLUME is not mounted" >&2
	echo "Close any tool using the Feather and double-tap RESET first" >&2
	exit 1
fi

dd if="$UF2_PATH" of="$BOOT_VOLUME/$(basename "$UF2_PATH")" bs=4k
sync

echo "Copied $UF2_PATH to $BOOT_VOLUME/"