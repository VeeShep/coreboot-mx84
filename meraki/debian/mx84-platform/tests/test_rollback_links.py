#!/usr/bin/python3
"""Exercise recovery isolation on real Linux links in private namespaces."""

import os
from pathlib import Path
import subprocess
import sys


def main():
    if len(sys.argv) == 1:
        subprocess.run([
            "unshare", "--net", "--mount", "--propagation", "private",
            sys.executable, str(Path(__file__).resolve()), "--child",
            str(os.stat("/proc/self/ns/net").st_ino),
            str(os.stat("/proc/self/ns/mnt").st_ino),
        ], check=True)
        return
    assert len(sys.argv) == 4 and sys.argv[1] == "--child"
    assert os.stat("/proc/self/ns/net").st_ino != int(sys.argv[2])
    assert os.stat("/proc/self/ns/mnt").st_ino != int(sys.argv[3])
    subprocess.run(["mount", "-t", "sysfs", "sysfs", "/sys"], check=True)
    from test_platform import port

    def ip(*args):
        subprocess.run(["ip", *args], check=True)

    config = port.load_config(Path(__file__).resolve().parents[1] / "root/etc/mx84/ports.conf")
    for trunk in port.router_trunks(config):
        ip("link", "add", trunk, "type", "dummy")
        ip("link", "set", trunk, "up")
    port.create_virtual_links(config)
    port.verify_virtual_links(config)
    for trunk in port.router_trunks(config):
        assert (Path('/sys/class/net') / trunk / 'mtu').read_text().strip() == '1516'
    port.disconnect_router_uplinks(config)
    for label in port.LABELS[1:]:
        interface = Path("/sys/class/net") / port.uplink(config, label)
        assert not (interface / "master").exists()
        assert int((interface / "flags").read_text(), 16) & 1 == 0
        assert not list((Path("/sys/class/net") / port.bridge(label) / "brif").iterdir())
    print("PASS: three CPU trunks and twelve isolated VLAN bridges created/verified; rollback detached all uplinks")


if __name__ == "__main__":
    main()
