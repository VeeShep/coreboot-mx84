#!/bin/sh
set -eu

if [ "$#" -ne 2 ] || [ ! -d "$1" ]; then
	echo "usage: prepare-source.sh PATCHED_LINUX_TREE NEW_OUTPUT_DIRECTORY" >&2
	exit 2
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
linux=${1%/}
output=${2%/}
parent=$(dirname -- "$output")
name=$(basename -- "$output")

[ -d "$parent" ] || { echo "output parent does not exist: $parent" >&2; exit 1; }
[ ! -e "$output" ] || { echo "refusing to overwrite: $output" >&2; exit 1; }
near=$(find "$parent" -maxdepth 1 -mindepth 1 -iname "$name" -print)
[ -z "$near" ] || { echo "case-conflicting output exists: $near" >&2; exit 1; }

for path in \
	drivers/gpio/gpio-ich.c \
	drivers/leds/leds-lp55xx-common.c \
	drivers/leds/leds-lp55xx-common.h \
	drivers/leds/leds-lp5562.c \
	drivers/net/ethernet/intel/igb/Makefile; do
	[ -f "$linux/$path" ] || { echo "missing patched kernel source: $path" >&2; exit 1; }
done

install -d "$output/igb"
install -m 0644 "$here/Makefile" "$here/dkms.conf" "$here/meraki-mx84.c" "$output/"
install -m 0644 "$linux/drivers/gpio/gpio-ich.c" "$output/gpio-ich.c"
install -m 0644 "$linux/drivers/leds/leds-lp55xx-common.c" "$output/leds-lp55xx-common.c"
install -m 0644 "$linux/drivers/leds/leds-lp55xx-common.h" "$output/leds-lp55xx-common.h"
install -m 0644 "$linux/drivers/leds/leds-lp5562.c" "$output/leds-lp5562.c"
for path in "$linux"/drivers/net/ethernet/intel/igb/*.[ch]; do
	install -m 0644 "$path" "$output/igb/"
done
install -m 0644 "$here/igb.Makefile" "$output/igb/Makefile"

echo "DKMS source staged at $output"
