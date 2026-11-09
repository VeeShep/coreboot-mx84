#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
identity="$here/root/usr/lib/udev/rules.d/68-mx84-preserve-phison.rules"
if [ ! -s "$identity" ] || ! grep -Eq 'ATTRS\{serial\}=="[[:alnum:]]+"' "$identity" || grep -q 'REPLACE_WITH_VERIFIED_SERIAL' "$identity"; then
    echo "Refusing build: configure a verified local Phison identity rule from preserve-phison.rules.example" >&2
    exit 1
fi
version=$(cat "$here/VERSION")
output=${1:-"$here/dist"}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM

mkdir -p "$work/DEBIAN" "$output"
cp -a "$here/root/." "$work/"
install -m 0755 "$here/../../buildroot/rootfs-overlay/usr/sbin/mx84-led" "$work/usr/sbin/mx84-led"
find "$work" -type d -name __pycache__ -prune -exec rm -rf {} +
sed "s/@VERSION@/$version/g" "$here/control" > "$work/DEBIAN/control"
install -m 0644 "$here/conffiles" "$work/DEBIAN/conffiles"
install -m 0755 "$here/postinst" "$work/DEBIAN/postinst"
install -m 0755 "$here/prerm" "$work/DEBIAN/prerm"
install -m 0755 "$here/postrm" "$work/DEBIAN/postrm"
find "$work/usr/sbin" "$work/usr/lib/mx84-platform" -type f -exec chmod 0755 {} +
chmod 0644 "$work/usr/lib/mx84-platform/interfaces-mgmt"
chmod 0755 "$work/usr/share/initramfs-tools/hooks/mx84-preserve-phison"
find "$work/usr/lib/systemd/system" -type f -exec chmod 0644 {} +
gzip -n -9 -c "$here/changelog" > "$work/usr/share/doc/mx84-platform/changelog.gz"
chmod 0644 "$work/usr/share/doc/mx84-platform/changelog.gz"
install -m 0644 "$here/README.md" "$work/usr/share/doc/mx84-platform/README.md"
chmod 0600 "$work/etc/mx84/pfrest.conf"

dpkg-deb --build --root-owner-group "$work" "$output/mx84-platform_${version}_all.deb"
