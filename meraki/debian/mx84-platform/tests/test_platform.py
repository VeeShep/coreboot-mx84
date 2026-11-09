#!/usr/bin/python3

import importlib.machinery
import importlib.util
import configparser
import hashlib
import io
import json
import sys
import subprocess
import tempfile
from unittest import mock
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent / "root"


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


ledd = load("mx84_ledd", ROOT / "usr/sbin/mx84-ledd")
reset = load("mx84_reset", ROOT / "usr/sbin/mx84-reset-button")
port = load("mx84_port", ROOT / "usr/sbin/mx84-port")
pfsense = load("mx84_pfsense", ROOT / "usr/sbin/mx84-pfsense-create")
backend = load("mx84_switch_backend", ROOT / "usr/lib/mx84-platform/mx84-switch-backend")


def test_hold_gate():
    config = reset.load_config(ROOT / "etc/mx84/reset-button.conf")
    assert config["INPUT_DEVICE"] == "/dev/input/by-path/platform-gpio-keys-polled-event"
    assert config["KEY_CODE"] == "408"
    assert config["HOLD_SECONDS"] == "2.0"

    gate = reset.HoldGate(2.0)
    gate.event(2, 0.0)  # key-repeat without a fresh press is ignored
    assert not gate.due(10.0)
    gate.event(1, 10.0)
    assert not gate.due(11.999)
    assert gate.due(12.0)
    assert not gate.due(20.0)
    gate.event(0, 20.0)
    gate.event(1, 21.0)
    assert gate.due(23.0)


def test_cpu_and_usb():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        proc = root / "proc"
        proc.mkdir()
        stat = proc / "stat"
        stat.write_text("cpu  100 0 100 800 0 0 0 0\n")
        meter = ledd.CpuMeter(proc)
        assert meter.sample() == 0.0
        stat.write_text("cpu  150 0 150 900 0 0 0 0\n")
        assert meter.sample() == 50.0

        meter = ledd.CpuMeter(proc)
        stat.write_text("cpu  100 0 0 100 0 0 0 0 100 0\n")
        meter.sample()
        stat.write_text("cpu  200 0 0 200 0 0 0 0 200 0\n")
        assert meter.sample() == 50.0, "KVM guest ticks must count only once"

        sysfs = root / "sys"
        block = sysfs / "bus/usb/devices/1-2/1-2:1.0/block/sda"
        block.mkdir(parents=True)
        stats = sysfs / "class/block/sda/stat"
        stats.parent.mkdir(parents=True)
        stats.write_text("1 0 0 0 2 0 0 0 0 0 0\n")
        usb = ledd.UsbState({"FRONT_USB_PATH": "1-2", "USB_ACTIVITY_HOLD_SECONDS": "1"}, sysfs)
        assert usb.classify(1.0) == ("storage", ["sda"])
        stats.write_text("2 0 0 0 2 0 0 0 0 0 0\n")
        assert usb.classify(2.0) == ("active", ["sda"])
        assert usb.classify(3.1) == ("storage", ["sda"])


def test_center_gradient_and_port_gate():
    class Platform:
        config = {"BOOT_RAINBOW_SECONDS": "0"}
        started = 0
        api_failures = 0
        api_healthy = True

        def infrastructure_ready(self): return True
        def critical_fault(self): return False
        def commissioned(self): return True
        def domain_state(self): return "running"
        def domain_in_start_grace(self, _state, _now): return False
        def check_api(self, _now): return True

    state, command = ledd.center_policy(Platform(), 50.0, "", 100.0)
    assert state == "online"
    assert command == ["rgb", "128", "0", "127"]

    grace = object.__new__(ledd.PlatformState)
    grace.config = {"PFSENSE_START_GRACE_SECONDS": "180"}
    grace.domain_last_state = None
    grace.domain_state_since = None
    assert grace.domain_in_start_grace("shut off", 10.0)
    assert grace.domain_in_start_grace("shut off", 189.9)
    assert not grace.domain_in_start_grace("shut off", 190.0)
    assert grace.domain_in_start_grace("running", 200.0)
    assert not grace.domain_in_start_grace("running", 380.0)
    assert grace.domain_in_start_grace("shut off", 201.0)
    assert port.endpoint_host("192.0.2.205:22") == "192.0.2.205"
    assert port.endpoint_host("[fe80::1]:22") == "fe80::1"

    config = port.load_config(HERE.parent / "root/etc/mx84/ports.conf")
    try:
        port.validate(config)
    except SystemExit as error:
        assert "physical port mapping is not verified" in str(error)
    else:
        raise AssertionError("unverified mapping must fail closed")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        backend = root / "switch-backend"
        backend.write_text("#!/bin/sh\nexit 0\n")
        backend.chmod(0o755)
        config["platform"]["mapping_verified"] = "yes"
        config["platform"]["cpu_mapping_verified"] = "yes"
        config["platform"]["switch_backend"] = str(backend)
        config["platform"]["switch_backend_sha256"] = hashlib.sha256(backend.read_bytes()).hexdigest()
        for number, label in enumerate(port.COPPER):
            config[label]["verified"] = "yes"
            config[label]["vsc_port"] = str(number)
        try:
            port.validate(config)
        except SystemExit as error:
            assert "management client subnet or gateway is unverified" in str(error)
        else:
            raise AssertionError("cutover must require a verified management return route")
        config["platform"]["management_client_subnet"] = "198.51.100.0/24"
        config["platform"]["management_gateway"] = "192.0.2.1"
        original_marker = port.SERIAL_MARKER
        port.SERIAL_MARKER = root / "serial-ready"
        try:
            assert port.validate(config) == backend
            try:
                port.validate(config, require_serial=True)
            except SystemExit as error:
                assert "serial recovery marker" in str(error)
            else:
                raise AssertionError("cutover must require serial recovery")
            port.SERIAL_MARKER.touch()
            assert port.validate(config, require_serial=True) == backend
        finally:
            port.SERIAL_MARKER = original_marker


def test_management_route_commands():
    config = port.load_config(HERE.parent / "root/etc/mx84/ports.conf")
    config["platform"]["management_client_subnet"] = "198.51.100.0/24"
    config["platform"]["management_gateway"] = "192.0.2.1"
    calls = []

    class Result:
        returncode = 0
        stdout = ""

    saved_run = port.run
    port.run = lambda command, **_kwargs: calls.append(command) or Result()
    try:
        port.configure_management_route(config)
        port.remove_management_route(config)
        assert calls == [
            ["ip", "route", "replace", "198.51.100.0/24", "via", "192.0.2.1", "dev", "mgmt0"],
            ["ip", "route", "del", "198.51.100.0/24", "via", "192.0.2.1", "dev", "mgmt0"],
        ]
        config["platform"]["management_gateway"] = "not-an-address"
        port.remove_management_route(config)
        assert len(calls) == 2
    finally:
        port.run = saved_run


def test_virtual_link_isolation_checks():
    config = port.load_config(HERE.parent / "root/etc/mx84/ports.conf")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        sys_net = root / "sys/class/net"
        for trunk in port.router_trunks(config):
            (sys_net / trunk).mkdir(parents=True)
        for label in port.LABELS[1:]:
            name = port.bridge(label)
            vlan_name = port.uplink(config, label)
            (sys_net / name / "brif" / vlan_name).mkdir(parents=True)
        (sys_net / "br-pfctl/brif").mkdir(parents=True)

        class Result:
            def __init__(self, returncode=0, stdout=""):
                self.returncode = returncode
                self.stdout = stdout

        saved = port.SYS_CLASS_NET, port.run
        port.SYS_CLASS_NET = sys_net
        port.run = lambda command, **_kwargs: Result(
            stdout="9: br-pfctl inet 172.31.84.1/30 scope global br-pfctl\n"
            if command[-1] == "br-pfctl" else ""
        )
        try:
            port.verify_virtual_links(config)
            port.run = lambda command, **_kwargs: Result(
                stdout=("9: br-pfctl inet 172.31.84.1/30 scope global br-pfctl\n"
                        if command[-1] == "br-pfctl" else
                        "4: br-wan1 inet 192.0.2.1/24 scope global br-wan1\n"
                        if command[-1] == "br-wan1" else "")
            )
            try:
                port.verify_virtual_links(config)
            except SystemExit as error:
                assert "host address present on br-wan1" in str(error)
            else:
                raise AssertionError("router-facing bridge address must fail isolation verification")
        finally:
            port.SYS_CLASS_NET, port.run = saved


def test_multiple_cpu_trunks_preserve_management():
    config = port.load_config(ROOT / "etc/mx84/ports.conf")
    calls = []
    with mock.patch.object(port, "run", side_effect=lambda command, **kwargs:
                           calls.append(command) or mock.Mock(returncode=1)):
        port.create_virtual_links(config)
    assert port.uplink(config, "WAN1") == "eth0.101"
    assert port.uplink(config, "WAN2") == "eth1.102"
    assert port.uplink(config, "LAN1") == "eth3.201"
    assert not any("mgmt0" in " ".join(command) for command in calls)
    for label in port.LABELS[1:]:
        assert ["ip", "link", "add", "link", port.cpu_interface(config, label),
                "name", port.uplink(config, label), "type", "vlan", "id",
                config[label]["vlan"]] in calls
    config["WAN1"]["cpu_interface"] = "mgmt0"
    calls.clear()
    with mock.patch.object(port, "run", side_effect=lambda command, **kwargs: calls.append(command)):
        try:
            port.create_virtual_links(config)
        except SystemExit as error:
            assert "unexpected CPU" in str(error)
        else:
            raise AssertionError("must reject Management as a router trunk before writes")
    assert not calls


def test_pfsense_xml():
    interfaces = "".join("<interface type='bridge'><mac address='00:00:00:00:00:00'/></interface>" for _ in pfsense.BRIDGES)
    xml = pfsense.customize_xml(f"<domain><os><boot dev='cdrom'/></os><on_reboot>destroy</on_reboot><devices>{interfaces}<memballoon model='virtio'/></devices></domain>")
    root = pfsense.ET.fromstring(xml)
    devices = root.find("devices")
    assert [boot.get("dev") for boot in root.findall("os/boot")] == ["hd", "cdrom"]
    assert root.findtext("on_reboot") == "restart"
    assert devices.find("memballoon").get("model") == "none"
    nics = devices.findall("interface")
    assert nics[0].find("mac").get("address") == "52:54:00:84:00:01"
    assert nics[-1].find("mac").get("address") == "52:54:00:84:00:0d"
    assert nics[10].find("link").get("state") == "down"
    assert nics[11].find("link").get("state") == "down"
    assert nics[12].find("link") is None
    assert [nic.find("alias").get("name") for nic in nics] == [
        "ua-mx84-" + label.lower() for label, _ in pfsense.BRIDGES
    ]
    repeated = pfsense.ET.fromstring(pfsense.customize_xml(xml))
    assert all(len(nic.findall("alias")) == 1 for nic in repeated.findall("devices/interface"))

    with tempfile.TemporaryDirectory() as temporary:
        disk = Path(temporary) / "pfsense.qcow2"
        disk.touch()
        pfsense.cleanup_failed_disk(disk, created=True, domain_defined=False)
        assert not disk.exists()
        disk.touch()
        pfsense.cleanup_failed_disk(disk, created=True, domain_defined=True)
        assert disk.exists()


def test_pfsense_creation_gates():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        iso = root / "installer.iso"
        iso.write_bytes(b"test installer")
        marker = root / "commissioned"
        marker.touch()
        disk = root / "images/pfsense.qcow2"
        calls = []
        interfaces = "".join("<interface/>" for _ in pfsense.BRIDGES)
        xml = f"<domain><os/><devices>{interfaces}</devices></domain>"

        def fake_run(command, **kwargs):
            calls.append(command)
            if command[0] == "qemu-img":
                disk.touch()
            return mock.Mock(stdout=xml if command[0] == "virt-install" else "")

        def mapped_path(value):
            return root if value == "/sys/class/net" else Path(value)

        for _, bridge in pfsense.BRIDGES:
            (root / bridge).mkdir()
        with mock.patch.object(pfsense, "DISK", disk), \
             mock.patch.object(pfsense, "COMMISSIONED", marker), \
             mock.patch.object(pfsense, "Path", side_effect=mapped_path), \
             mock.patch.object(pfsense.os, "geteuid", return_value=0), \
             mock.patch.object(sys, "argv", ["create", str(iso), pfsense.sha256(iso)]), \
             mock.patch.object(pfsense.subprocess, "run", return_value=mock.Mock(returncode=1)), \
             mock.patch.object(pfsense, "run", side_effect=RuntimeError("network invalid")):
            try:
                pfsense.main()
            except RuntimeError as error:
                assert str(error) == "network invalid"
            else:
                raise AssertionError("network verification must gate disk creation")
            assert not disk.exists()
            assert not disk.parent.exists()

            pfsense.run.side_effect = fake_run
            pfsense.main()
            assert calls[0] == ["/usr/sbin/mx84-port", "verify"]
            generation = next(command for command in calls if command[0] == "virt-install")
            assert "--print-xml=1" in generation and "--print-xml" not in generation
            boot = generation[generation.index("--boot") + 1]
            assert "firmware.feature0.name=secure-boot,firmware.feature0.enabled=no" in boot
            assert "firmware.feature1.name=enrolled-keys,firmware.feature1.enabled=no" in boot
            assert calls[-1] == ["systemctl", "start", "mx84-pfsense.service"]
            assert not any("enable" in command for command in calls)
            assert all(command[1:3] == ["--connect", "qemu:///system"]
                       for command in calls if command[0] == "virsh")
            assert disk.exists()

            disk.unlink()
            for failure, preserve in (("virt-install", False), ("define", False), ("start", True)):
                calls.clear()

                def fail_at_stage(command, **kwargs):
                    if failure in command:
                        raise RuntimeError(failure)
                    return fake_run(command, **kwargs)

                pfsense.run.side_effect = fail_at_stage
                try:
                    pfsense.main()
                except RuntimeError as error:
                    assert str(error) == failure
                else:
                    raise AssertionError(f"expected failure at {failure}")
                assert disk.exists() == preserve
                if preserve:
                    disk.unlink()


def test_center_grace_resets_on_running():
    platform = object.__new__(ledd.PlatformState)
    platform.config = {"BOOT_RAINBOW_SECONDS": "0", "PFSENSE_START_GRACE_SECONDS": "180"}
    platform.started = 0
    platform.domain_last_state = None
    platform.domain_state_since = None
    platform.api_failures = 0
    with mock.patch.object(platform, "critical_fault", return_value=False), \
         mock.patch.object(platform, "commissioned", return_value=True), \
         mock.patch.object(platform, "check_api", return_value=True) as api, \
         mock.patch.object(platform, "domain_state", return_value="shut off") as state:
        assert ledd.center_policy(platform, 0, "", 10)[0] == "pfsense-starting"
        assert ledd.center_policy(platform, 0, "", 190)[0] == "pfsense-stopped"
        state.return_value = "running"
        api.return_value = False
        platform.api_failures = 3
        assert ledd.center_policy(platform, 0, "", 200)[0] == "pfsense-starting"
        assert ledd.center_policy(platform, 0, "", 380)[0] == "api-fault"
        api.return_value = True
        assert ledd.center_policy(platform, 0, "", 390)[0] == "online"
        platform.api_healthy = True
        state.return_value = "shut off"
        assert ledd.center_policy(platform, 0, "", 391)[0] == "pfsense-starting"
        assert not platform.api_healthy


def test_eth0_retirement_and_restore():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        network = root / "etc/network"
        interfaces = network / "interfaces.d/mx84"
        interfaces.parent.mkdir(parents=True)
        original = b"# recovery path\nauto eth0\niface eth0 inet dhcp\n"
        interfaces.write_bytes(original)
        saved = (port.NETWORK_DIR, port.RECOVERY_INTERFACES, port.RECOVERY_INTERFACES_PATH, port.run)
        port.NETWORK_DIR = network
        port.RECOVERY_INTERFACES = root / "state/recovery-eth0.interfaces"
        port.RECOVERY_INTERFACES_PATH = root / "state/recovery-eth0.path"

        class Result:
            returncode = 0
            stdout = ""

        port.run = lambda *_args, **_kwargs: Result()
        try:
            port.retire_legacy_eth0()
            assert interfaces.read_text().startswith("# eth0 DHCP retired")
            port.restore_legacy_eth0()
            assert interfaces.read_bytes() == original
        finally:
            port.NETWORK_DIR, port.RECOVERY_INTERFACES, port.RECOVERY_INTERFACES_PATH, port.run = saved


def test_first_apply_ownership_and_confirm_order():
    config = port.load_config(HERE.parent / "root/etc/mx84/ports.conf")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        sys_net = root / "sys/class/net"
        for trunk in port.router_trunks(config):
            (sys_net / trunk).mkdir(parents=True)
            (sys_net / trunk / "mtu").write_text("1500\n")

        class Result:
            returncode = 1
            stdout = ""

        saved = (port.SYS_CLASS_NET, port.OWNED_LINKS, port.TRUNK_MTU, port.ROLLBACK_MARKER,
                 port.require_root, port.verify, port.retire_legacy_eth0, port.run)
        port.SYS_CLASS_NET = sys_net
        port.OWNED_LINKS = root / "state/links"
        port.TRUNK_MTU = root / "state/mtu"
        port.ROLLBACK_MARKER = root / "run/rollback"
        port.run = lambda *_args, **_kwargs: Result()
        try:
            port.prepare_link_ownership(config)
            assert set(port.OWNED_LINKS.read_text().splitlines()) == set(port.managed_links(config))
            assert json.loads(port.TRUNK_MTU.read_text()) == {"eth0": 1500, "eth1": 1500, "eth3": 1500}

            port.require_root = lambda: None
            port.verify = lambda _config: True
            port.retire_legacy_eth0 = lambda: (_ for _ in ()).throw(AssertionError("eth0 retired before rollback marker validation"))
            try:
                port.confirm(config, require_ssh=False)
            except SystemExit as error:
                assert "no armed rollback" in str(error)
            else:
                raise AssertionError("confirmation without rollback marker must fail")
        finally:
            (port.SYS_CLASS_NET, port.OWNED_LINKS, port.TRUNK_MTU, port.ROLLBACK_MARKER,
             port.require_root, port.verify, port.retire_legacy_eth0, port.run) = saved


def test_switch_backend_apply_verify_and_flat():
    class FakeSwitch:
        def __init__(self):
            self.ports, self.vlans = backend.flat_state()

        def __call__(self, *arguments):
            arguments = tuple(map(str, arguments))
            if arguments == ("vlan", "read"):
                lines = []
                for vlan, members in sorted(self.vlans.items()):
                    lines.append(f"VLAN: {vlan} members: {','.join(backend.ranges(members))}")
                return "\n".join(lines) + "\n\nret: 0\n"
            if arguments[:2] == ("vlan", "read"):
                vlan = int(arguments[2])
                if vlan not in self.vlans:
                    return "There is no vlan information to show.\n\nret: 0\n"
                return f"VLAN: {vlan} members: {','.join(backend.ranges(self.vlans[vlan]))}\n\nret: 0\n"
            if arguments[0] == "vlan":
                vlan = int(arguments[1])
                members = backend.parse_members(arguments[2])
                current = self.vlans.setdefault(vlan, set())
                if arguments[3] == "member":
                    current.update(members)
                else:
                    current.difference_update(members)
                    if not current:
                        self.vlans.pop(vlan)
                return "ret: 0\n"
            port_number = int(arguments[1])
            if arguments[2] == "read":
                state = self.ports[port_number]
                return (
                    f"Port {port_number}, state: 1Gfdx, ifilt: {state['ifilt']}, "
                    f"PVID: {state['pvid']}, UVID: {state['uvid']}, "
                    f"tags: VTSS_VLAN_FRAME_ALL, learn: {state['learn']}, "
                    f"type: {state['type']}, max_len: {state['max_len']}, "
                    f"max_tags: {state['max_tags']}\n\nret: 0\n"
                )
            field, value = arguments[2:4]
            if field == "type":
                value = {
                    "0": "VTSS_VLAN_PORT_TYPE_UNAWARE",
                    "1": "VTSS_VLAN_PORT_TYPE_C",
                    "2": "VTSS_VLAN_PORT_TYPE_S",
                    "3": "VTSS_VLAN_PORT_TYPE_S_CUSTOM",
                }[value]
            self.ports[port_number][field] = value
            return "ret: 0\n"

    config = configparser.ConfigParser()
    config.read(ROOT / "etc/mx84/ports.conf")
    config["platform"]["mapping_verified"] = "yes"
    for number, label in enumerate(backend.COPPER):
        config[label]["verified"] = "yes"
        config[label]["vsc_port"] = str(number)

    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "ports.conf"
        with path.open("w") as stream:
            config.write(stream)
        physical = backend.load_config(path)

    fake = FakeSwitch()
    saved_ctl = backend.ctl
    backend.ctl = fake
    try:
        backend.apply_profile(physical)
        wanted_ports, wanted_vlans = backend.desired_state(physical)
        assert backend.state_matches(wanted_ports, wanted_vlans)
        assert 1 not in fake.vlans
        assert fake.vlans[101] == {1, 13}
        assert fake.vlans[102] == {2, 14}
        assert fake.vlans[399] == {0, 15}
        for cpu in (13, 14, 16):
            assert fake.ports[cpu]["max_tags"] == "1"
        assert fake.vlans[301] == {16}
        assert fake.vlans[302] == {16}
        assert fake.ports[11]["pvid"] == "1"
        assert fake.ports[12]["pvid"] == "1"

        fake.ports[16]["max_len"] = "1518"
        differences = backend.state_differences(wanted_ports, wanted_vlans)
        assert differences == [
            "port 16 expected {'learn': 'true', 'ifilt': 'true', 'pvid': '1', "
            "'uvid': '1', 'type': 'VTSS_VLAN_PORT_TYPE_C', 'max_len': '1522', "
            "'max_tags': '1'}, got {'learn': 'true', 'ifilt': 'true', 'pvid': '1', "
            "'uvid': '1', 'type': 'VTSS_VLAN_PORT_TYPE_C', 'max_len': '1518', "
            "'max_tags': '1'}"
        ]
        fake.ports[16]["max_len"] = "1522"

        backend.restore_flat()
        flat_ports, flat_vlans = backend.flat_state()
        assert backend.state_matches(flat_ports, flat_vlans)

        fake.vlans = {7: {0}}
        try:
            backend.apply_profile(physical)
        except backend.BackendError as error:
            assert "unknown switch state" in str(error)
        else:
            raise AssertionError("backend must refuse an unknown starting state")
    finally:
        backend.ctl = saved_ctl


def test_physical_management_carrier():
    platform = ledd.PlatformState({})
    carrier = mock.Mock(return_value=True)
    api = {"load_config": mock.Mock(return_value={"MGMT": 0}), "read_carrier": carrier}
    with mock.patch.object(platform, "critical_fault", return_value=False) as fault, \
         mock.patch.object(ledd.runpy, "run_path", return_value=api) as loader, \
         mock.patch.object(ledd, "run", return_value=mock.Mock(returncode=0, stdout="IPv4 present")) as address:
        assert platform.management() == "healthy"
        carrier.assert_called_with(0)
        carrier.return_value = False
        assert platform.management() == "no-carrier", "must ignore the permanently-up CPU carrier and stale lease"
        address.return_value.stdout = ""
        carrier.return_value = True
        assert platform.management() == "no-dhcp"
        loader.assert_called_once()
        carrier.side_effect = RuntimeError("vendor socket unavailable")
        assert platform.management() == "fault"
        carrier.side_effect = subprocess.TimeoutExpired("vendor", 3)
        assert platform.management() == "fault"
        carrier.side_effect = None
        address.return_value.returncode = 1
        assert platform.management() == "fault"
        fault.return_value = True
        carrier.return_value = False
        assert platform.management() == "fault"

    with mock.patch.object(backend, "ctl") as control:
        for state, expected in [("discon", False), ("Down", False), ("1Gfdx", True),
                                ("100Mhdx", True), ("10Mfdx", True)]:
            control.return_value = f"Port 0, state: {state}, ifilt: true\nret: 0\n"
            assert backend.read_carrier(0) == expected
        for output in ["Port 15, state: 1Gfdx,", "Port 0, state: unknown,", "ret: 0"]:
            control.return_value = output
            try:
                backend.read_carrier(0)
            except backend.BackendError:
                pass
            else:
                raise AssertionError("unknown/mismatched carrier output must fail closed")
        control.reset_mock()
        try:
            backend.read_carrier(15)
        except backend.BackendError:
            pass
        else:
            raise AssertionError("CPU uplink cannot supply physical jack carrier")
        control.assert_not_called()


def test_switch_backend_control_timeout():
    with mock.patch.object(backend, "verify_vendor"), \
         mock.patch.object(backend.subprocess, "run", side_effect=subprocess.TimeoutExpired("vendor", 3)) as call:
        try:
            backend.ctl("port", 0, "read")
        except backend.BackendError:
            pass
        else:
            raise AssertionError("vendor timeout must be a recoverable backend failure")
        assert call.call_args.kwargs["timeout"] == 3


def test_switch_backend_checks_embedded_return_code():
    class Result:
        returncode = 0
        stdout = "Usage: vlan (...)\n\nret: -22\n"
        stderr = ""

    saved_verify = backend.verify_vendor
    saved_run = backend.subprocess.run
    backend.verify_vendor = lambda: None
    backend.subprocess.run = lambda *_args, **_kwargs: Result()
    try:
        try:
            backend.ctl("vlan", "read", "4096")
        except backend.BackendError as error:
            assert "ret: -22" in str(error)
        else:
            raise AssertionError("vtssctl reports failures in output while exiting zero")
    finally:
        backend.verify_vendor = saved_verify
        backend.subprocess.run = saved_run


def test_switch_backend_write_modes_require_root():
    saved_argv = backend.sys.argv
    saved_geteuid = backend.os.geteuid
    backend.sys.argv = ["mx84-switch-backend", "apply", "/etc/mx84/ports.conf"]
    backend.os.geteuid = lambda: 1000
    try:
        try:
            backend.main()
        except SystemExit as error:
            assert "requires root" in str(error)
        else:
            raise AssertionError("direct switch writes must require root")
    finally:
        backend.sys.argv = saved_argv
        backend.os.geteuid = saved_geteuid


def test_rollback_isolates_guest_before_flat():
    config = port.load_config(ROOT / "etc/mx84/ports.conf")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        sys_net = root / "net"
        marker = root / "commissioned"
        backend_path = root / "backend"
        backend_path.write_text("#!/bin/sh\nexit 0\n")
        backend_path.chmod(0o755)
        config["platform"]["switch_backend"] = str(backend_path)
        for label in port.LABELS[1:]:
            vlan = port.uplink(config, label)
            (sys_net / vlan).mkdir(parents=True)
            (sys_net / port.bridge(label) / "brif" / vlan).mkdir(parents=True)
        calls = []
        fail_disconnect = False

        def fake_run(command, **kwargs):
            calls.append(command)
            assert not marker.exists(), "boot gate must close before recovery commands"
            if fail_disconnect and command[-1] == "nomaster":
                raise subprocess.CalledProcessError(1, command)
            return mock.Mock(returncode=0)

        def mapped_path(value):
            path = Path(value)
            return root / "run" / path.relative_to("/run/mx84") if path.is_relative_to("/run/mx84") else path

        with mock.patch.multiple(port, SYS_CLASS_NET=sys_net, COMMISSIONED=marker,
                                 ROLLBACK_MARKER=root / "rollback"), \
             mock.patch.object(port, "Path", side_effect=mapped_path), \
             mock.patch.object(port, "require_root"), \
             mock.patch.object(port, "remove_management_route"), \
             mock.patch.object(port, "restore_local_network"), \
             mock.patch.object(port, "restore_legacy_eth0"), \
             mock.patch.object(port, "run", side_effect=fake_run):
            marker.touch()
            port.rollback(config)
            flat_index = next(index for index, call in enumerate(calls) if "flat" in call)
            detached = [call for call in calls[:flat_index] if call[-1] == "nomaster"]
            assert len(detached) == 12
            assert ["systemctl", "disable", "mx84-pfsense.service"] in calls[:flat_index]
            assert ["virsh", "--connect", "qemu:///system", "shutdown", "pfsense"] in calls[:flat_index]

            calls.clear()
            marker.touch()
            fail_disconnect = True
            try:
                port.rollback(config)
            except subprocess.CalledProcessError:
                pass
            else:
                raise AssertionError("failed disconnection must prevent flattening")
            assert not any("flat" in call for call in calls)

            fail_disconnect = False
            calls.clear()
            (sys_net / "br-wan1/brif/unknown-physical").mkdir()
            try:
                port.rollback(config)
            except SystemExit as error:
                assert "unexpected member" in str(error)
            else:
                raise AssertionError("unexpected physical member must prevent flattening")
            assert not any("flat" in call for call in calls)


def test_api_health_contract():
    credentials = {
        "API_URL": "https://172.31.84.2/api/v2/status/system",
        "API_KEY": "test-only-key", "API_CA": "/test/ca.pem",
    }
    platform = ledd.PlatformState({"API_CHECK_SECONDS": "5"})
    healthy = {"code": 200, "status": "ok", "data": {"uptime": "1 minute", "cpu_count": 2}}
    body = healthy

    def response(request, **kwargs):
        assert request.get_header("X-api-key") == "test-only-key"
        stream = io.BytesIO(json.dumps(body).encode())
        stream.status = 200
        return stream

    context = object()
    fetch = mock.Mock(side_effect=response)
    with mock.patch.object(ledd, "load_config", return_value=credentials), \
         mock.patch.object(ledd.ssl, "create_default_context", return_value=context) as tls, \
         mock.patch.object(ledd.urllib.request, "HTTPSHandler") as https, \
         mock.patch.object(ledd.urllib.request, "build_opener", return_value=mock.Mock(open=fetch)):
        assert platform.check_api(10)
        tls.assert_called_with(cafile="/test/ca.pem")
        https.assert_called_with(context=context)
        assert platform.check_api(11)
        assert fetch.call_count == 1, "cached checks must not count as new API attempts"
        for now, invalid in ((15, {}), (20, {"code": 403, "status": "error", "data": {}}),
                             (25, {"code": 200, "status": "ok", "data": []})):
            body = invalid
            assert not platform.check_api(now)
        assert platform.api_failures == 3
        body = healthy
        assert platform.check_api(30)
        assert platform.api_failures == 0
        fetch.side_effect = ValueError("invalid JSON")
        assert not platform.check_api(35)
        assert platform.api_failures == 1
        fetch.reset_mock()
        credentials["API_URL"] = "http://172.31.84.2/api/v2/status/system"
        assert not platform.check_api(40)
        fetch.assert_not_called()
    assert ledd.NoRedirect().redirect_request(None, None, 302, "", {}, "http://example.invalid") is None


def test_led_subprocess_failures():
    platform = ledd.PlatformState({})
    led = ledd.Led()
    for error in (subprocess.TimeoutExpired("virsh", 5), FileNotFoundError("missing command")):
        with mock.patch.object(ledd.subprocess, "run", side_effect=error):
            assert not platform.service_active("mx84-switch.service")
            assert platform.domain_state() == "absent"
            assert not led.set("blue")
            assert not led.last, "failed LED writes must be retried"
    with mock.patch.object(ledd.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stdout="running\n")):
        assert platform.service_active("mx84-switch.service")
        assert platform.domain_state() == "running"
        assert led.set("blue")
        assert led.last["center"] == ("blue",)


if __name__ == "__main__":
    test_led_subprocess_failures()
    test_hold_gate()
    test_cpu_and_usb()
    test_center_gradient_and_port_gate()
    test_management_route_commands()
    test_virtual_link_isolation_checks()
    test_multiple_cpu_trunks_preserve_management()
    test_pfsense_xml()
    test_pfsense_creation_gates()
    test_center_grace_resets_on_running()
    test_eth0_retirement_and_restore()
    test_first_apply_ownership_and_confirm_order()
    test_switch_backend_apply_verify_and_flat()
    test_switch_backend_checks_embedded_return_code()
    test_physical_management_carrier()
    test_switch_backend_control_timeout()
    test_switch_backend_write_modes_require_root()
    test_rollback_isolates_guest_before_flat()
    test_api_health_contract()
    print("mx84-platform Python checks passed")
