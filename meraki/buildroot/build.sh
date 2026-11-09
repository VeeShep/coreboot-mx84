#!/bin/sh
set -eu

version=2026.05.1
archive="buildroot-$version.tar.xz"
archive_sha256=ae7f706f087b9ae9083a10a587368dfbf53103c28bf81c2d690198dc4090cb58
slot_size=$((0x700000))

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=${MX84_BUILD_DIR:-"$HOME/mx84-build"}
source_dir="$work/buildroot-$version"
output_dir="$work/output"

mkdir -p "$work"
if [ ! -f "$work/$archive" ]; then
	wget -O "$work/$archive.part" "https://buildroot.org/downloads/$archive"
	mv "$work/$archive.part" "$work/$archive"
fi
printf '%s  %s\n' "$archive_sha256" "$work/$archive" | sha256sum -c -

if [ ! -d "$source_dir" ]; then
	tar -C "$work" -xf "$work/$archive"
fi

mkdir -p "$source_dir/board/mx84"
cp "$here/linux.config" "$source_dir/board/mx84/linux.config"
mkdir -p "$source_dir/board/mx84/patches"
cp -R "$here/patches/." "$source_dir/board/mx84/patches/"
mkdir -p "$source_dir/board/mx84/rootfs-overlay"
cp -R "$here/rootfs-overlay/." "$source_dir/board/mx84/rootfs-overlay/"

make -C "$source_dir" O="$output_dir" BR2_DEFCONFIG="$here/mx84_defconfig" defconfig
make -C "$source_dir" O="$output_dir" -j"$(nproc)"

images="$output_dir/images"
cp "$here/mx84.its" "$images/mx84.its"
(
	cd "$images"
	"$output_dir/host/bin/mkimage" -f mx84.its mx84-test.itb
)

fit_size=$(stat -c %s "$images/mx84-test.itb")
if [ "$fit_size" -gt "$slot_size" ]; then
	printf 'FIT image is %s bytes; slot limit is %s\n' "$fit_size" "$slot_size" >&2
	exit 1
fi

"$output_dir/host/bin/dumpimage" -l "$images/mx84-test.itb"
sha256sum "$images/bzImage" "$images/rootfs.cpio.gz" "$images/mx84-test.itb"
printf 'FIT uses %s of %s bytes; output: %s\n' "$fit_size" "$slot_size" "$images/mx84-test.itb"
