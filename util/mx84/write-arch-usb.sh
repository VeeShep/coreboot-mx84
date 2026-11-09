#!/bin/sh
set -eu

expected_iso_sha256=4e82dced1c4fd3e498b22a853f8db2a4d262d32b97e7e07d97390d9e425ffe5e
iso=${1:?usage: sudo write-arch-usb.sh ARCH_ISO /dev/disk/by-id/usb-DISK}
disk=${2:?usage: sudo write-arch-usb.sh ARCH_ISO /dev/disk/by-id/usb-DISK}

case "$disk" in
	/dev/disk/by-id/usb-*) ;;
	*) printf 'Refusing non-USB by-id path: %s\n' "$disk" >&2; exit 1 ;;
esac

device=$(readlink -f -- "$disk")
name=$(basename -- "$device")
test -b "$device"
test "$(cat "/sys/class/block/$name/removable")" = 1
udevadm info --query=property --name="$device" | grep -qx 'ID_BUS=usb'
printf '%s  %s\n' "$expected_iso_sha256" "$iso" | sha256sum -c -

iso_bytes=$(stat -c %s "$iso")
start_sector=$(( ((iso_bytes + 1048575) / 1048576) * 2048 ))
disk_sectors=$(cat "/sys/class/block/$name/size")
test "$disk_sectors" -gt "$((start_sector + 2048))"

lsblk -nrpo NAME,MOUNTPOINT "$device" | while read -r node mountpoint; do
	test -z "$mountpoint" || umount "$node"
done

dd if="$iso" of="$device" bs=4M conv=fsync status=progress
printf 'start=%s, type=83\n' "$start_sector" | sfdisk --append "$device"
partprobe "$device"
udevadm settle

partition="${device}3"
test -b "$partition"
mkfs.ext4 -F -L MX84DATA "$partition"

mountpoint=$(mktemp -d)
trap 'umount "$mountpoint" 2>/dev/null || true; rmdir "$mountpoint"' EXIT HUP INT TERM
mount "$partition" "$mountpoint"
mkdir "$mountpoint/scripts" "$mountpoint/logs" "$mountpoint/notes"
printf '%s\n' \
	'MX84 persistent test data' \
	'' \
	'From Arch Linux:' \
	'  mkdir -p /mnt/mx84' \
	'  mount -L MX84DATA /mnt/mx84' \
	'' \
	'Save reusable commands in /mnt/mx84/scripts and captures in /mnt/mx84/logs.' \
	>"$mountpoint/README.txt"
chown "${SUDO_UID:-0}:${SUDO_GID:-0}" \
	"$mountpoint/scripts" "$mountpoint/logs" "$mountpoint/notes" \
	"$mountpoint/README.txt"
sync
umount "$mountpoint"
rmdir "$mountpoint"
trap - EXIT HUP INT TERM

fdisk -l "$device"
blkid "$partition"
