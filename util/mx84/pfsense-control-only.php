<?php
// Run after the host's authenticated API/TLS checks pass. Management is on Debian.
require_once 'config.inc';
require_once 'interfaces.inc';
require_once 'filter.inc';
if (config_get_path('interfaces/lan/if') !== 'vtnet12' ||
    config_get_path('interfaces/lan/ipaddr') !== '172.31.84.2' ||
    config_get_path('interfaces/wan/if') !== 'vtnet0') {
    throw new RuntimeException('Refusing unexpected interface topology');
}
foreach (config_get_path('filter/rule', []) as $rule) {
    if (!in_array($rule['descr'] ?? '', ['Default allow LAN to any rule', 'Default allow LAN IPv6 to any rule'])) {
        throw new RuntimeException('Refusing to replace non-default firewall rules');
    }
}
$backup = '/cf/conf/config.xml.pre-control-only-20261108';
if (file_exists($backup) || !copy('/cf/conf/config.xml', $backup)) {
    throw new RuntimeException('Backup already exists or could not be created');
}
chmod($backup, 0600);
umask(0077);
$accepted = '/var/run/mx84-control-accepted';
@unlink($accepted);
file_put_contents('/root/mx84-control-rollback.sh', "#!/bin/sh\nsleep 120\n" .
    "test -e $accepted && exit 0\ncp -p $backup /cf/conf/config.xml\n" .
    "rm -f /tmp/config.cache\n/etc/rc.reload_all\n");
exec('/usr/sbin/daemon -f -p /var/run/mx84-control-rollback.pid /bin/sh /root/mx84-control-rollback.sh', $output, $code);
if ($code !== 0) { throw new RuntimeException('Could not arm guest rollback'); }
$rules = [];
foreach (['443', '22', 'icmp'] as $i => $port) {
    $rule = [
        'type' => 'pass', 'interface' => 'lan', 'ipprotocol' => 'inet',
        'protocol' => $port === 'icmp' ? 'icmp' : 'tcp',
        'source' => ['address' => '172.31.84.1'],
        'destination' => ['address' => '172.31.84.2'],
        'descr' => 'MX84 private host control ' . $port, 'tracker' => (string)(time() + $i),
    ];
    if ($port !== 'icmp') { $rule['destination']['port'] = $port; }
    $rules[] = $rule;
}
interface_bring_down('wan');
config_del_path('interfaces/wan');
config_del_path('dhcpd');
config_set_path('filter/rule', $rules);
config_set_path('system/webgui/noantilockout', true);
write_config('MX84 control-only guest; physical roles unassigned; SSH/HTTPS restricted to host');
filter_configure();
echo "Control-only configuration applied. Verify fresh HTTPS/SSH, then touch $accepted within 120 seconds.\n";
