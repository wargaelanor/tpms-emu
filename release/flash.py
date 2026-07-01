#!/usr/bin/env python3
"""
TPMS Emulator Flash Script v3
- Auto-detects ESP32-C3 or ESP8266 and flashes appropriate firmware
- Supports Flash Encryption (--encrypt flag, ESP32-C3 only)
- Supports offline firmware encryption for production (ESP32-C3 only)
"""

import subprocess
import sys
import os
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# When running as PyInstaller onefile .exe:
# - Bundled data (firmware .bin) lives in sys._MEIPASS temp dir
# - sys.argv[0] is the original EXE location
if getattr(sys, 'frozen', False):
    SCRIPT_DIR = getattr(sys, '_MEIPASS',
                         os.path.dirname(os.path.abspath(sys.argv[0])))

def _find_python():
    """Find real Python interpreter (needed when running from PyInstaller EXE)."""
    if not getattr(sys, 'frozen', False):
        return sys.executable
    import shutil
    for name in ("python", "python3"):
        p = shutil.which(name)
        if p:
            return p
    return sys.executable

ESP_VID_PIDS = {
    "esp32c3": "303A:1001",
    "esp8266": ["10C4:EA60", "1A86:7523", "1A86:55D4", "1A86:55D3", "0403:6001"],
}

FLASH_LAYOUT = {
    "esp32c3": {
        "chip": "esp32c3",
        "baud": "460800",
        "mode": "dio",
        "addrs": [("0x0000", "bootloader.bin"), ("0x8000", "partitions.bin"), ("0x10000", "firmware_esp32c3.bin")],
    },
    "esp8266": {
        "chip": "esp8266",
        "baud": "230400",
        "mode": "dout",
        "addrs": [("0x0000", "firmware_esp8266.bin")],
    },
}

def find_esp_port():
    """Return (port, chip_type) or (None, None)."""
    try:
        import serial.tools.list_ports
        for p in serial.tools.list_ports.comports():
            if p.vid and p.pid:
                hwid = "{:04X}:{:04X}".format(p.vid, p.pid)
                if hwid == ESP_VID_PIDS["esp32c3"]:
                    return p.device, "esp32c3"
                if hwid in ESP_VID_PIDS.get("esp8266", []):
                    return p.device, "esp8266"
    except ImportError:
        pass
    return None, None

def find_key_file(chip_type="esp32c3"):
    project_dir = os.path.join(SCRIPT_DIR, "..")
    env_name = "esp32-c3-devkitm-1" if chip_type == "esp32c3" else "esp8266-nodemcu"
    candidates = [
        os.path.join(SCRIPT_DIR, "flash_encryption_key.bin"),
        os.path.join(project_dir, ".pio", "build", env_name, "flash_encryption_key.bin"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None

def flash_firmware(port, chip_type="esp32c3", encrypt=False, keyfile=None):
    layout = FLASH_LAYOUT.get(chip_type, FLASH_LAYOUT["esp32c3"])
    script_dir = SCRIPT_DIR

    # Verify all files exist
    for addr, fname in layout["addrs"]:
        fpath = os.path.join(script_dir, fname)
        if not os.path.exists(fpath):
            print(f"ERROR: {fname} not found in {script_dir}!")
            return False

    baud = layout["baud"]
    python_exe = _find_python()
    cmd = [
        python_exe, "-m", "esptool",
        "--chip", layout["chip"],
        "--port", port,
        "--baud", baud,
        "write_flash", "-z",
        "--flash_mode", layout["mode"],
        "--flash_freq", "80m" if chip_type == "esp32c3" else "40m",
        "--flash_size", "4MB",
    ]

    if encrypt:
        if chip_type != "esp32c3":
            print("ERROR: Flash Encryption only supported on ESP32-C3")
            return False
        print("  [Flash Encryption ENABLED]")
        cmd.append("--encrypt")
        if keyfile:
            cmd.extend(["--keyfile", keyfile])
    else:
        print("  [Normal mode — no encryption]")

    for addr, fname in layout["addrs"]:
        cmd.extend([addr, os.path.join(script_dir, fname)])

    print(f"\nFlashing {port} ({chip_type})...")
    for addr, fname in layout["addrs"]:
        print(f"  {fname} @ {addr} ({(baud)} baud)")
    if keyfile:
        print(f"  Key file : {keyfile}")
    print()

    try:
        subprocess.run(cmd, check=True)
        print("\nFlash OK!")
        if encrypt:
            print("WARNING: Flash Encryption eFuse has been BURNED!")
            print("This chip will ONLY accept encrypted firmware from now on.")
            print("Keep your encryption key safe!")
        return True
    except subprocess.CalledProcessError as e:
        print(f"\nFlash FAILED: {e}")
        return False
    except FileNotFoundError:
        print("\nERROR: esptool not installed. Run: pip install esptool")
        return False

def encrypt_firmware_offline(keyfile, chip_type="esp32c3", output_name="firmware_encrypted.bin"):
    """Pre-encrypt firmware for production (ESP32-C3 only, no chip needed)"""
    if chip_type != "esp32c3":
        print("ERROR: Offline encryption only supported for ESP32-C3")
        return False
    layout = FLASH_LAYOUT["esp32c3"]
    firmware_in = os.path.join(SCRIPT_DIR, layout["addrs"][-1][1])
    output = os.path.join(SCRIPT_DIR, output_name)

    if not os.path.exists(keyfile):
        print(f"ERROR: key file not found: {keyfile}")
        return False

    python_exe = _find_python()
    cmd = [
        python_exe, "-m", "esptool",
        "--chip", "esp32c3",
        "encrypt_data",
        "--keyfile", keyfile,
        firmware_in,
        output
    ]

    print(f"Encrypting firmware ({firmware_in})...")
    print(f"  Key file : {keyfile}")
    print(f"  Output   : {output}")

    try:
        subprocess.run(cmd, check=True)
        print("\nEncryption OK! Encrypted binary saved.")
        print("Flash with:")
        print(f"  esptool.py --chip esp32c3 --port COM8 write_flash -z --encrypt 0x10000 {output}")
        return True
    except subprocess.CalledProcessError as e:
        print(f"\nEncryption FAILED: {e}")
        return False

def check_security(port, chip_type="esp32c3"):
    """Check chip security info (flash encryption status)"""
    baud = FLASH_LAYOUT.get(chip_type, FLASH_LAYOUT["esp32c3"])["baud"]
    python_exe = _find_python()
    cmd = [
        python_exe, "-m", "esptool",
        "--chip", chip_type,
        "--port", port,
        "--baud", baud,
        "get_security_info"
    ]
    try:
        result = subprocess.run(cmd, check=True, capture_output=True)
        output = result.stdout.decode('utf-8', errors='replace')
        print(output)
        return output
    except:
        return None

def list_ports():
    """Print all serial ports and exit."""
    try:
        import serial.tools.list_ports
        ports = serial.tools.list_ports.comports()
        if not ports:
            print("No serial ports found.")
            return
        print("Available ports:")
        for p in ports:
            hwid = ""
            if p.vid and p.pid:
                hwid = f"  [{p.vid:04X}:{p.pid:04X}]"
            label = p.description if p.description else p.device
            print(f"  {p.device}: {label}{hwid}")
    except ImportError:
        print("pyserial not installed. Run: pip install pyserial")

def main():
    parser = argparse.ArgumentParser(description="TPMS Emulator Flash Tool v3")
    parser.add_argument("--chip", "-c", type=str, default=None,
                       choices=["esp32c3", "esp8266"],
                       help="Chip type (auto-detect if not specified)")
    parser.add_argument("--encrypt", action="store_true",
                       help="Enable Flash Encryption (ESP32-C3 only, BURNS eFuse — irreversible!)")
    parser.add_argument("--keyfile", type=str, default=None,
                       help="Path to flash encryption key file")
    parser.add_argument("--encrypt-offline", type=str, nargs="?", const="firmware_encrypted.bin",
                       metavar="OUTPUT.bin",
                       help="Encrypt firmware offline using key file (ESP32-C3 only, no chip needed)")
    parser.add_argument("--info", action="store_true",
                       help="Show chip security info")
    parser.add_argument("--port", "-p", type=str, default=None,
                       help="Serial port (auto-detect if not specified)")
    parser.add_argument("--list-ports", action="store_true",
                       help="List all serial ports and exit")
    args = parser.parse_args()

    print("=" * 50)
    print("  TPMS Emulator Flash Tool v3")
    print("=" * 50)

    # List ports mode
    if args.list_ports:
        list_ports()
        return

    # Handle offline encryption mode
    if args.encrypt_offline:
        chip_type = args.chip or "esp32c3"
        keyfile = args.keyfile or find_key_file(chip_type)
        if not keyfile:
            print("\nERROR: No encryption key found!")
            print("Generate one with: esptool.py --chip esp32c3 generate_flash_encryption_key key.bin")
            return
        encrypt_firmware_offline(keyfile, chip_type, args.encrypt_offline)
        return

    # Auto-detect chip and port
    port = args.port
    chip_type = args.chip

    if not chip_type or not port:
        auto_port, auto_chip = find_esp_port()
        if not port and auto_port:
            port = auto_port
        if not chip_type and auto_chip:
            chip_type = auto_chip

    # If still not found, scan
    if not port:
        print("\nScanning for ESP device...")
        try:
            import serial.tools.list_ports
            ports = serial.tools.list_ports.comports()
            for p in ports:
                hwid = ""
                if p.vid and p.pid:
                    hwid = f"  [{p.vid:04X}:{p.pid:04X}]"
                print(f"  {p.device}: {p.description}{hwid}")
            if not ports:
                print("  No serial ports found!")
                return
        except ImportError:
            print("  pyserial not installed. Run: pip install pyserial")

        if not chip_type:
            print("\nChip type not specified. Use --chip esp32c3 or --chip esp8266")
            print("Or use --list-ports to see available ports.")
            return

        if not port:
            print(f"\nNo {chip_type} found. Specify port with --port COMx")
            return

    # Default chip_type if still not set
    if not chip_type:
        chip_type = "esp32c3"

    print(f"\nFound {chip_type} on {port}")

    # Show security info if requested
    if args.info:
        print("\n--- Security Info ---")
        check_security(port, chip_type)
        return

    # Find key file for encrypted mode
    keyfile = args.keyfile or (find_key_file(chip_type) if args.encrypt else None)

    # Check if chip already has encryption before enabling
    if args.encrypt:
        print("\n⚠  WARNING: Flash Encryption will BURN eFuse!")
        print("   This is IRREVERSIBLE on this chip.")
        print("   After this, the chip will ONLY accept encrypted firmware.\n")
        print("   Press Enter to continue, or Ctrl+C to cancel...")
        try:
            input()
        except KeyboardInterrupt:
            print("\nCancelled.")
            return

    # Flash
    if flash_firmware(port, chip_type=chip_type, encrypt=args.encrypt, keyfile=keyfile):
        print(f"\nDone! {chip_type} will restart.")
        print("Connect to WiFi: TPMS (password: 12345678)")
        print("Open: http://192.168.4.1")

        if args.encrypt:
            print("\nIMPORTANT: Backup your encryption key!")
            if keyfile:
                print(f"  Key: {keyfile}")
            print("  Without this key you CANNOT flash this chip again.")
    else:
        print("\nFlash failed. Check connections and try again.")

if __name__ == "__main__":
    main()
