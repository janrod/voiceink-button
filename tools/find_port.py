#!/usr/bin/env python3
"""Prints the /dev/cu.* port of the USB device whose serial number is the given MAC, or exits 1.

    python3 tools/find_port.py AA:BB:CC:DD:EE:FF
    python3 tools/find_port.py --list      # every connected ESP32-S3, with its MAC and port

Ports are named after the USB socket, not the device, so a port name alone can point at another
board. The ESP32-S3 USB serial number is its MAC: "AA:BB:CC:DD:EE:FF" in the ROM / JTAG port, and
"AABBCCDDEEFF" when the dongle firmware runs its own USB. Both forms match.
"""
import plistlib
import subprocess
import sys

ESPRESSIF_VID = 0x303A


def norm(s):
    return s.replace(":", "").upper()


def ports(node):
    if "IOCalloutDevice" in node:
        yield node["IOCalloutDevice"]
    for child in node.get("IORegistryEntryChildren", []):
        yield from ports(child)


def devices():
    tree = plistlib.loads(subprocess.check_output(["ioreg", "-a", "-r", "-l", "-c", "IOUSBHostDevice"]))
    for dev in tree:
        cu = [p for p in ports(dev) if p.startswith("/dev/cu.")]
        yield dev, cu


def as_mac(serial):
    s = norm(serial)
    return ":".join(s[i:i + 2] for i in range(0, 12, 2)) if len(s) == 12 else serial


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        sys.exit(2)
    if sys.argv[1] == "--list":
        for dev, cu in devices():
            if dev.get("idVendor") == ESPRESSIF_VID:
                print(f"{as_mac(dev.get('USB Serial Number', '?'))}  {dev.get('USB Product Name', '?'):28}  {' '.join(cu) or '-'}")
        return
    want = norm(sys.argv[1])
    found = [p for dev, cu in devices() if norm(dev.get("USB Serial Number", "")) == want for p in cu]
    if len(found) != 1:
        print(f"find_port: {'no' if not found else 'several'} port(s) for {sys.argv[1]}", file=sys.stderr)
        sys.exit(1)
    print(found[0])


main()
