#!/usr/bin/env python3
"""
Generate Flash Encryption Key for TPMS Emulator
Run ONCE before first encrypted flash.
Keep the generated key.bin file SECRET!
"""

import subprocess
import sys
import os

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    key_path = os.path.join(script_dir, "flash_encryption_key.bin")

    print("=" * 50)
    print("  TPMS Emulator — Generate Flash Encryption Key")
    print("=" * 50)

    if os.path.exists(key_path):
        print(f"\nKey already exists: {key_path}")
        yn = input("Overwrite? (y/N): ")
        if yn.lower() != 'y':
            print("Cancelled.")
            return

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32c3",
        "generate_flash_encryption_key", key_path
    ]

    print(f"\nGenerating key to: {key_path}")
    try:
        subprocess.run(cmd, check=True)
        print("\nKey generated successfully!")
        print(f"  Key file: {key_path}")
        print("\n⚠  KEEP THIS FILE SECRET!")
        print("   Without it, you CANNOT flash encrypted chips.")
        print("\nNext steps:")
        print(f"  1. python flash.py --encrypt --keyfile {key_path}")
        print(f"  2. Keep {key_path} in a safe place (not in git!)")
    except subprocess.CalledProcessError as e:
        print(f"\nFAILED: {e}")
    except FileNotFoundError:
        print("\nERROR: esptool not installed. Run: pip install esptool")

if __name__ == "__main__":
    main()
