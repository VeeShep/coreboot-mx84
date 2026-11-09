#!/bin/sh
# Hardware acceptance on Fry; temporary namespace/veth only, no switch or host IP edits.
set -eu
task_label=${1:-WAN1}
case "$task_label" in
    WAN1|WAN2|LAN[1-8]) task_bridge="br-$(printf '%s' "$task_label" | tr '[:upper:]' '[:lower:]')" ;;
    *) echo "usage: test-copper-path.sh WAN1|WAN2|LAN1..LAN8 (never MGMT)" >&2; exit 2 ;;
esac
[ "$(id -u)" -eq 0 ] || { echo "run as root on Fry" >&2; exit 1; }
mx84-port verify >/dev/null
task_ns=mx84-port-test
task_host=mx84-port-probe
test ! -e "/run/netns/$task_ns"
test ! -e "/sys/class/net/$task_host"
task_test=$(mktemp -d /var/tmp/mx84-port-test.XXXXXX)
task_ns_created=0
task_link_created=0
cleanup() {
    if [ "$task_link_created" = 1 ]; then ip link del "$task_host" 2>/dev/null || true; fi
    if [ "$task_ns_created" = 1 ]; then ip netns del "$task_ns" 2>/dev/null || true; fi
    rm -f "$task_test/dhcp.sh" "$task_test/udhcpc.pid"
    rmdir "$task_test" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
cat > "$task_test/dhcp.sh" <<'DHCP_SCRIPT'
#!/bin/sh
set -eu
case "$1" in
    deconfig) ip -4 address flush dev "$interface" ;;
    bound|renew)
        prefix=$(python3 -c 'import ipaddress,os; print(ipaddress.IPv4Network("0.0.0.0/"+os.environ["subnet"]).prefixlen)')
        ip address replace "$ip/$prefix" dev "$interface"
        if [ -n "${router:-}" ]; then ip route replace default via "${router%% *}" dev "$interface"; fi
        ;;
esac
DHCP_SCRIPT
chmod 700 "$task_test/dhcp.sh"
ip netns add "$task_ns"
task_ns_created=1
ip link add "$task_host" type veth peer name wanprobe netns "$task_ns"
task_link_created=1
sysctl -q -w "net.ipv6.conf.$task_host.disable_ipv6=1"
ip link set "$task_host" master "$task_bridge"
ip link set "$task_host" up
ip -n "$task_ns" link set lo up
ip -n "$task_ns" link set wanprobe address 52:54:00:84:fe:01 up
timeout 20 ip netns exec "$task_ns" busybox udhcpc -f -n -q -t 3 -T 2 -i wanprobe -p "$task_test/udhcpc.pid" -s "$task_test/dhcp.sh"
ip -n "$task_ns" -4 -brief address show wanprobe
task_gateway=$(ip -n "$task_ns" -4 route show default | awk '{print $3; exit}')
test -n "$task_gateway"
ip netns exec "$task_ns" ping -c 2 -W 2 "$task_gateway"
ip netns exec "$task_ns" ping -M do -s 1472 -c 3 -W 2 "$task_gateway"
printf 'PASS: %s DHCP, bidirectional gateway traffic and 1500-byte IPv4 packets; cleanup follows\n' "$task_label"

