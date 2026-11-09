#!/bin/sh
set -eu

commit=b52ca86e094d19b58e2304417787e96b940e39c6
source_url=https://github.com/coreboot/seabios.git
stock_sha256=6d7f25597f5c4da293b7393265e4d0c21a92394387febcc71de729651d54a6a5
elf_sha256=7d12272f92ca114d6843bf4b9894b421401e6d9e28a16451155b2109ea612e1c
rom_sha256=a87f05019c0178b37a6c8d18f3db8a7fd55915bc3170a8aad9bf5bdabfa0e06a

if [ "$#" -ne 2 ]; then
	printf 'usage: %s STOCK_ROM OUTPUT_ROM\n' "$0" >&2
	exit 2
fi

stock_rom=$1
output_rom=$2
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$here/../.." && pwd)
work=${MX84_SEABIOS_BUILD_DIR:-"$HOME/mx84-seabios-build"}
source_dir="$work/seabios"
output_dir="$work/seabios-out"
cbfstool_dir="$work/cbfstool"
cbfstool="$cbfstool_dir/cbfstool"

if [ -e "$output_rom" ]; then
	printf 'output already exists: %s\n' "$output_rom" >&2
	exit 1
fi

actual_stock_sha256=$(sha256sum "$stock_rom" | cut -d' ' -f1)
if [ "$actual_stock_sha256" != "$stock_sha256" ]; then
	printf 'stock ROM hash is %s; expected %s\n' \
		"$actual_stock_sha256" "$stock_sha256" >&2
	exit 1
fi

mkdir -p "$work"
if [ ! -d "$source_dir/.git" ]; then
	git clone "$source_url" "$source_dir"
fi
if [ -n "$(git -C "$source_dir" status --porcelain --untracked-files=no)" ]; then
	printf 'SeaBIOS source has tracked changes: %s\n' "$source_dir" >&2
	exit 1
fi
if ! git -C "$source_dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
	git -C "$source_dir" fetch origin "$commit"
fi
git -C "$source_dir" checkout --detach "$commit"

cp "$here/seabios.config" "$source_dir/.config"
mkdir -p "$output_dir"
make -s -C "$source_dir" OUT="$output_dir/" olddefconfig
make -s -C "$source_dir" OUT="$output_dir/" -j"$(nproc)"

actual_elf_sha256=$(sha256sum "$output_dir/bios.bin.elf" | cut -d' ' -f1)
if [ "$actual_elf_sha256" != "$elf_sha256" ]; then
	printf 'SeaBIOS ELF hash is %s; expected %s\n' \
		"$actual_elf_sha256" "$elf_sha256" >&2
	exit 1
fi

mkdir -p "$cbfstool_dir/lzma/C"
make -s -C "$repo_root/util/cbfstool" obj="$cbfstool_dir" "$cbfstool"

temporary_rom=$(mktemp "$output_rom.tmp.XXXXXX")
trap 'rm -f -- "$temporary_rom"' EXIT HUP INT TERM
cp "$stock_rom" "$temporary_rom"
"$cbfstool" "$temporary_rom" remove -n fallback/payload
"$cbfstool" "$temporary_rom" add-int -n etc/sercon-port -i 0x2f8
"$cbfstool" "$temporary_rom" add-int -n etc/boot-menu-wait -i 5000
"$cbfstool" "$temporary_rom" add-int -n etc/screen-and-debug -i 0
"$cbfstool" "$temporary_rom" add-int -n etc/threads -i 0
"$cbfstool" "$temporary_rom" add-payload \
	-f "$output_dir/bios.bin.elf" -n fallback/payload -c lzma

actual_rom_sha256=$(sha256sum "$temporary_rom" | cut -d' ' -f1)
if [ "$actual_rom_sha256" != "$rom_sha256" ]; then
	printf 'generated ROM hash is %s; expected %s\n' \
		"$actual_rom_sha256" "$rom_sha256" >&2
	exit 1
fi

mv "$temporary_rom" "$output_rom"
trap - EXIT HUP INT TERM
printf '%s  %s\n' "$actual_elf_sha256" "$output_dir/bios.bin.elf"
printf '%s  %s\n' "$actual_rom_sha256" "$output_rom"
