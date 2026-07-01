#!/usr/bin/env python3
"""
TPMS Emulator License Key Generator

Usage:
    python keygen.py <DEVICE_ID>

Device ID is 12 hex characters from the WiFi MAC address,
shown in the web UI License section.

Output: 8-character hex license key
"""

import sys
import hmac
import hashlib

# Must match LICENSE_SECRET in src/main.cpp
LICENSE_SECRET = bytes([
    0x2A, 0x7C, 0xB9, 0x41, 0xD3, 0xE5, 0x8F, 0x06,
    0x9C, 0x15, 0xF7, 0xBA, 0x40, 0x62, 0x83, 0x1E
])


def generate_key(device_id: str) -> str:
    h = hmac.new(LICENSE_SECRET, device_id.upper().encode(), hashlib.sha256).digest()
    return h[:4].hex().upper()


def main():
    if len(sys.argv) < 2:
        print("Usage: python keygen.py <DEVICE_ID>")
        print("Example: python keygen.py A1B2C3D4E5F6")
        sys.exit(1)

    device_id = sys.argv[1].upper().strip()
    if len(device_id) != 12 or not all(c in '0123456789ABCDEF' for c in device_id):
        print("Error: DEVICE_ID must be exactly 12 hex characters (0-9, A-F)")
        print("Example: A1B2C3D4E5F6")
        sys.exit(1)

    key = generate_key(device_id)
    print(f"Device ID: {device_id}")
    print(f"License Key: {key}")


if __name__ == "__main__":
    main()
