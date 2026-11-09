<?php
// Run as root inside the newly installed pfSense CE 2.9.0 guest, after pfREST 2.11.0.
require_once 'RESTAPI/autoloader.inc';
use RESTAPI\Models\CertificateAuthorityGenerate;
use RESTAPI\Models\CertificateGenerate;
use RESTAPI\Models\User;
use RESTAPI\Models\RESTAPIKey;
use RESTAPI\Models\RESTAPISettings;
use RESTAPI\Models\RESTAPIAccessListEntry;

if (trim(file_get_contents('/etc/version')) !== '2.9.0-RELEASE' ||
    config_get_path('interfaces/lan/if') !== 'vtnet12' ||
    config_get_path('interfaces/lan/ipaddr') !== '172.31.84.2') {
    throw new RuntimeException('Refusing unexpected guest version/control topology');
}
umask(0077);
$material = '/root/mx84-api-material.json';
$ca = null;
foreach (config_get_path('ca', []) as $entry) {
    if ($entry['descr'] === 'MX84 control CA') { $ca = $entry; break; }
}
if ($ca === null) {
    (new CertificateAuthorityGenerate(data: [
        'descr' => 'MX84 control CA', 'keytype' => 'ECDSA', 'ecname' => 'prime256v1',
        'digest_alg' => 'sha256', 'lifetime' => 3650, 'dn_commonname' => 'MX84 control CA',
        // OpenSSL rejects the package model's empty country/DN defaults.
        'dn_country' => 'US', 'dn_state' => 'Local', 'dn_city' => 'Local',
        'dn_organization' => 'MX84', 'dn_organizationalunit' => 'Private control',
    ]))->create();
    foreach (config_get_path('ca', []) as $entry) {
        if ($entry['descr'] === 'MX84 control CA') { $ca = $entry; break; }
    }
}
if ($ca === null) { throw new RuntimeException('CA generation failed'); }
$cert = null;
foreach (config_get_path('cert', []) as $entry) {
    if ($entry['descr'] === 'MX84 private control HTTPS') { $cert = $entry; break; }
}
if ($cert === null) {
    (new CertificateGenerate(data: [
        'descr' => 'MX84 private control HTTPS', 'caref' => $ca['refid'],
        'keytype' => 'ECDSA', 'ecname' => 'prime256v1', 'digest_alg' => 'sha256',
        'lifetime' => 825, 'dn_commonname' => 'fry-pfsense.home.arpa', 'type' => 'server',
        'dn_dns_sans' => ['fry-pfsense.home.arpa', 'localhost'],
        'dn_ip_sans' => ['172.31.84.2', '127.0.0.1'],
    ]))->create();
    foreach (config_get_path('cert', []) as $entry) {
        if ($entry['descr'] === 'MX84 private control HTTPS') { $cert = $entry; break; }
    }
}
if ($cert === null) { throw new RuntimeException('Server certificate generation failed'); }
$users = User::query(['name' => 'mx84-health']);
if (!$users->exists()) {
    (new User(data: [
        'name' => 'mx84-health', 'password' => bin2hex(random_bytes(32)),
        'descr' => 'API-only MX84 status monitor', 'priv' => ['api-v2-status-system-get'],
    ]))->create();
}
foreach (config_get_path('system/user', []) as $user) {
    if ($user['name'] === 'mx84-health' && $user['priv'] !== ['api-v2-status-system-get']) {
        throw new RuntimeException('Health user has unexpected privileges');
    }
}
if (!file_exists($material)) {
    $key = new RESTAPIKey(data: ['descr' => 'Debian LED health monitor', 'length_bytes' => 32]);
    $key->client->username = 'mx84-health';
    $key->create();
    $pem = base64_decode($ca['crt'], true);
    if (!str_starts_with($pem ?: '', '-----BEGIN CERTIFICATE-----')) {
        throw new RuntimeException('Invalid public CA export');
    }
    if (file_put_contents($material, json_encode(['key' => $key->key->value, 'ca' => $pem], JSON_THROW_ON_ERROR)) === false) {
        throw new RuntimeException('Unable to save API material');
    }
    chmod($material, 0600);
}
$settings = new RESTAPISettings();
$settings->read();
config_set_path($settings->config_path . '/access_list/item', []);
(new RESTAPIAccessListEntry(data: [
    'type' => 'allow', 'weight' => 0, 'network' => '172.31.84.1/32',
    'users' => ['mx84-health'], 'descr' => 'Only Debian private control host',
]))->create();
(new RESTAPISettings(data: [
    'enabled' => true, 'read_only' => true, 'keep_backup' => true,
    'login_protection' => true, 'allowed_interfaces' => ['lan'],
    'auth_methods' => ['KeyAuth'], 'expose_sensitive_fields' => false,
]))->update();
config_set_path('system/webgui/ssl-certref', $cert['refid']);
config_set_path('system/webgui/protocol', 'https');
write_config('MX84 private TLS, API-only status user, host-only ACL and read-only API');
echo "Configured API health user, ACL, read-only settings and private TLS; key export is root-only.\n";
