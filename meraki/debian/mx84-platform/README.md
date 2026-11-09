# mx84-platform

This local Debian package contains the open MX84 integration only. It does not
contain Cisco/Meraki binaries, firmware, keys, or configuration.

The vendor initializer restricts forwarding with PVLANs as well as VLANs.
WAN1 uses eth0/VSC13, WAN2 eth1/VSC14, LAN/SFP eth3/VSC16, and Management
stays untagged on mgmt0/VSC15. Router bridges remain addressless and their
names and VM NIC identities do not change. All three router trunks use MTU
1516; VLAN links and bridges use 1500. Never bridge Management into a VM.

Build on Debian with:

```sh
./build.sh
```

The package enables the LED and reset-button services for the next boot.  The
reset handler uses the physically verified stable
`/dev/input/by-path/platform-gpio-keys-polled-event` path and requires a
two-second hold of `KEY_RESTART` (code 408).  Network writes remain blocked
until copper and CPU-port mappings, serial recovery, and the exact switch
backend hash are all recorded in `/etc/mx84/ports.conf`.

The package also tells `systemd-logind` to ignore its immediate default reboot
action for `KEY_RESTART`; otherwise a short reset-button press can reboot the
host before the two-second handler gets a chance to enforce its hold policy.

While the LED daemon is running, any manual center, HA, USB, or power command
becomes the sole persistent override; all automatic LED writes pause until
`mx84-led auto` or reboot.

Use `mx84-port list` and `mx84-port status` for read-only inspection. Run
`mx84-port apply` only from serial with the recovery marker present. It arms a
two-minute flat-VLAN rollback before writing. Reconnect through the physical
MGMT jack and run `mx84-port confirm`; confirmation is refused unless an
established SSH session terminates on `mgmt0`. The original `eth0` DHCP stanza
is saved before retirement and restored by `mx84-port rollback`.

Rollback first removes the network-commissioned boot gate, disables both
systemd and libvirt guest autostart, and takes router VLAN uplinks down and
detaches them from their bridges before restoring flat VLAN 1. An unexpected
physical bridge member or failed disconnection prevents flattening. Guest
shutdown is requested gracefully; an unresponsive guest may remain running
with its physical paths disconnected. Shutdown completion must be checked on
the live appliance before recovery is declared complete.

Confirmation also verifies that every router bridge contains its one intended
VLAN link, has no host address, and has no unexpected non-VM member. The
isolated control bridge must contain no physical member and exactly
`172.31.84.1/30`; otherwise the automatic flat rollback remains armed.

The cutover also requires an explicit `management_client_subnet` and
`management_gateway`. `mx84-port apply` installs that return route through
`mgmt0`; flat rollback removes it. The packaged defaults are `UNVERIFIED`, so
no route is inferred from the build host or written before site values are
recorded.

Install the owner-extracted static switch tools separately with:

```sh
mx84-vendor-install /path/to/preserved/stock/rootfs
```

Never commit the resulting `/opt/mx84/vendor` files.

Before building, copy `preserve-phison.rules.example` to
`root/usr/lib/udev/rules.d/68-mx84-preserve-phison.rules` and replace
`REPLACE_WITH_VERIFIED_SERIAL` with the preserved device's verified serial.
The configured rule is deliberately ignored by Git; keep it with private
recovery material. The build refuses a missing or unconfigured rule.

The preservation rule `68-mx84-preserve-phison.rules` matches the configured unit's
internal USB VID/PID and serial exactly, sets kernel read-only mode, and blocks
the LVM udev autoactivation path. It does not rely on `/dev/sdb` numbering or
apply to arbitrary USB storage. Adapt the verified identity before using this
package on another unit. Existing active LVM mappings are not deactivated by
package installation. The initramfs hook includes the rule and blockdev for
early boot; existing boot images must be rebuilt explicitly and inspected.
Keep previous known-good images and require console recovery for boot testing.
The tracked `mx84-switch.service` verifies that runtime's hashes before it
starts the daemon and no longer depends on `/root/mx84-stock-safe`.

After the isolated network is commissioned and an official installer has been
downloaded and hashed, define the direct-KVM guest with:

```sh
mx84-pfsense-create /path/to/installer.iso EXPECTED_SHA256
```

The VM uses two vCPUs, 2048 MiB fixed RAM, a 32 GiB sparse qcow2 disk, Q35/OVMF,
VirtIO, no balloon, and deterministic `vtnet0` through `vtnet12` ordering. The
host service disables checksum and segmentation offloads on the VM's `vnet*`
paths after each start. The matching pfSense VirtIO offload options must still
be verified in the guest before routing production traffic.

If XML generation or domain definition fails, a disk created by that attempt
is removed so the same verified installer can be retried. Once the domain is
defined, its disk is preserved on later service/start failures for diagnosis.

Creation verifies the current switch/bridge profile before allocating a disk and
uses the system libvirt connection explicitly. It starts the installer service
for this session without enabling it at boot. Enable `mx84-pfsense.service`
only after guest installation, isolated control/API health, and disk-boot
acceptance. Custom NIC aliases use libvirt's required `ua-` prefix.

`/etc/mx84/pfrest.conf` is mode `0600`. Configure the control URL, API key, and
the pfSense CA certificate path; TLS verification is mandatory and is never
silently disabled.

HA carrier comes from the verified physical MGMT port in `ports.conf`, queried
through the existing vendor control API. The I354 `mgmt0` carrier is a CPU
uplink and does not prove a front cable is present. Unknown mappings, malformed
link state or bounded vendor-control failures produce HA fault rather than a
false green/off indication. Physical unplug/replug acceptance is still required.

### Commissioned API on Fry (2026-11-08)

pfREST 2.11.0's CE 2.9.0 asset is pinned to SHA-256
`b1b8c7c6b365c777dd5c438c72d2afa15fb8e96b305e6361f817e12fe9f1aa2e`.
This upstream package is unofficial. Its published retirement notice requires
future maintenance planning; pfSense updates also remove unofficial packages.
Do not change API providers silently or install a mismatched release.

After preserving guest config, `util/mx84/pfsense-api-setup.php` runs inside the
guest against the installed upstream models. It creates a private CA and
IP-SAN server certificate, API-only `mx84-health` user, 32-byte API key,
status-GET privilege, source ACL 172.31.84.1/32, LAN-only API, read-only mode,
and login protection. Certificate private keys remain in pfSense configuration;
full configuration backups also contain them and must stay root-only. Transfer
the root-only `/root/mx84-api-material.json` over trusted SSH to root-only host
storage, install its public CA at `/etc/mx84/pfsense-ca.pem`, and populate
`pfrest.conf` without logging the key. The server certificate lasts 825 days;
renew it before expiry and keep the CA trust path valid.

`util/mx84/pfsense-control-only.php` is a one-time fresh-install migration.
It refuses custom firewall rules, backs up config, arms a 120-second rollback,
unassigns temporary WAN, and permits only host .1 to control .2 TCP22/443 and
ICMP. Verify fresh TLS/API and SSH before touching the printed acceptance
marker. All physical VM interfaces remain available for manual role assignment.

Autostart is controlled by `mx84-pfsense.service`, with network readiness
checked on every start. Keep libvirt's independent autostart disabled so it
cannot bypass that gate. Enable the systemd service only after authenticated
health, negative authorization, guest disk boot, and LED acceptance pass.
Libvirt maintenance restarts do not stop the VM. Starting the pfSense service
reuses a running domain after readiness checks; paused or shutting-down domains
fail visibly and require inspection. Stopping the pfSense service still requests
a graceful guest shutdown.
API loss produces yellow; restoring valid TLS/API access restores the gradient.
`mx84-led status` shows policy and hardware channel values; manual colors last
until `mx84-led auto` or reboot. A stopped VM clears cached API health.

Recovery uses Management SSH to Debian and `virsh console pfsense` for the
guest. Disable the pfSense systemd service to revoke autostart. Guest
`/cf/conf/config.xml.pre-api-20261108` and
`config.xml.pre-control-only-20261108` are preserved root-only snapshots.
Restoring old guest config may also restore its old certificate/WAN assignment;
recheck TLS and interfaces afterward. Do not flatten or restart the host switch
to repair a guest API problem. Keep site acceptance evidence in private notes.
