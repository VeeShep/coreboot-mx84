#!/usr/bin/python3
"""Linux-only integration check; uses libvirt's in-memory test driver."""

import xml.etree.ElementTree as ET
import libvirt

from test_platform import pfsense


def main():
    connection = libvirt.open("test:///default")
    try:
        root = ET.fromstring(connection.lookupByName("test").XMLDesc(0))
        root.find("name").text = "mx84-xml-validation"
        root.remove(root.find("uuid"))
        devices = root.find("devices")
        for interface in devices.findall("interface"):
            devices.remove(interface)
        for _label, bridge in pfsense.BRIDGES:
            interface = ET.SubElement(devices, "interface", {"type": "bridge"})
            ET.SubElement(interface, "source", {"bridge": bridge})
            ET.SubElement(interface, "model", {"type": "virtio"})
        xml = pfsense.customize_xml(ET.tostring(root, encoding="unicode"))
        domain = connection.defineXMLFlags(xml, libvirt.VIR_DOMAIN_DEFINE_VALIDATE)
        actual = ET.fromstring(domain.XMLDesc(0))
        interfaces = actual.findall("devices/interface")
        assert len(interfaces) == 13
        for index, (interface, (label, bridge)) in enumerate(zip(interfaces, pfsense.BRIDGES)):
            assert interface.find("source").get("bridge") == bridge
            assert interface.find("mac").get("address") == f"52:54:00:84:00:{index + 1:02x}"
            assert interface.find("alias").get("name") == "ua-mx84-" + label.lower()
        assert actual.find("devices/memballoon").get("model") == "none"
        print("PASS: real libvirt test driver accepts thirteen bridges, MACs, aliases, and no balloon")
        print("Not tested: QEMU/KVM boot, PCI enumeration, bridge forwarding, or pfSense")
    finally:
        connection.close()


if __name__ == "__main__":
    main()
