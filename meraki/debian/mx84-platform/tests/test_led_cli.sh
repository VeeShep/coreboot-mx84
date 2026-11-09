#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cli="$here/../../../buildroot/rootfs-overlay/usr/sbin/mx84-led"
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

leds="$temporary/leds"
gpios="$temporary/gpios"
run="$temporary/run"
mkdir -p "$leds" "$gpios/gpiochip512" "$run"
printf gpio_ich > "$gpios/gpiochip512/label"
for name in 'tricolor0:red' 'tricolor0:green' 'tricolor0:blue' 'power:white'; do
	mkdir -p "$leds/$name"
	printf 0 > "$leds/$name/brightness"
done
for offset in 39 40 50 58 59; do
	mkdir -p "$gpios/gpio$((512 + offset))"
	printf 0 > "$gpios/gpio$((512 + offset))/value"
	printf low > "$gpios/gpio$((512 + offset))/direction"
done

env="MX84_LED_TESTING=1 MX84_LED_SYSFS=$leds MX84_GPIO_SYSFS=$gpios MX84_LED_RUN=$run"
env $env "$cli" hex 8040FF
[ "$(cat "$leds/tricolor0:red/brightness")" = 128 ]
[ "$(cat "$leds/tricolor0:green/brightness")" = 64 ]
[ "$(cat "$leds/tricolor0:blue/brightness")" = 255 ]
printf '%s\n' "$$" > "$run/daemon.pid"
env $env "$cli" rgb 1 2 3 >/dev/null
[ "$(cat "$run/override")" = 'rgb 1 2 3' ]
[ "$(cat "$leds/tricolor0:red/brightness")" = 128 ]
env $env "$cli" ha green >/dev/null
[ "$(cat "$run/override")" = 'ha green' ]
[ "$(cat "$gpios/gpio562/direction")" = low ]
[ "$(cat "$gpios/gpio570/direction")" = low ]
env $env "$cli" auto >/dev/null
[ ! -e "$run/override" ]
rm "$run/daemon.pid"
if env $env "$cli" rgb 256 0 0 >/dev/null 2>&1; then
	echo "invalid RGB unexpectedly succeeded" >&2
	exit 1
fi
env $env "$cli" ha both
[ "$(cat "$gpios/gpio562/direction")" = high ]
[ "$(cat "$gpios/gpio570/direction")" = high ]
env $env "$cli" usb green
[ "$(cat "$gpios/gpio551/direction")" = high ]
[ "$(cat "$gpios/gpio552/direction")" = low ]
echo "mx84-led fake-sysfs checks passed"
