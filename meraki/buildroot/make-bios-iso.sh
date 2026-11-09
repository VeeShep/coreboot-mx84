#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
kernel=${1:-"$here/output/bzImage-milesfix1"}
initramfs=${2:-"$here/output/rootfs-milesfix1.cpio.gz"}
output=${3:-"$here/output/mx84-rescue-bios.iso"}
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM

test -f "$kernel"
test -f "$initramfs"
test ! -e "$output"
command -v grub-mkrescue >/dev/null
command -v xorriso >/dev/null

mkdir -p "$stage/boot/grub"
cp "$kernel" "$stage/boot/bzImage"
cp "$initramfs" "$stage/boot/rootfs.cpio.gz"

cat >"$stage/boot/grub/grub.cfg" <<'EOF'
serial --unit=1 --speed=115200 --word=8 --parity=no --stop=1
terminal_input serial
terminal_output serial
set timeout=5
set default=0

menuentry 'MX84 Buildroot rescue' {
	linux /boot/bzImage console=ttyS1,115200n8 earlyprintk=serial,ttyS1,115200 earlycon=uart8250,io,0x2f8,115200n8 loglevel=8 rdinit=/sbin/init gpio-ich.initial_dir=0xFCFFFFEF,0xF785FFF3 gpio-ich.initial_vals=0x02000010,0x0078000C MERAKI_BOARD=pocahontas
	initrd /boot/rootfs.cpio.gz
}
EOF

grub-mkrescue -o "$output" "$stage"
if ! xorriso -indev "$output" -report_el_torito plain 2>&1 |
	grep -Eq 'El Torito boot img :.* BIOS +y '; then
	rm -f "$output"
	printf 'grub-mkrescue did not create a BIOS boot image; install grub-pc-bin\n' >&2
	exit 1
fi
sha256sum "$output"
