#!/usr/bin/env python3
"""
TPMS Emulator — Graphical Tool (License + Flash)

Tabs:
  🔑 License  — generate license key from Device ID (HMAC-SHA256)
  ⚡ Flash    — flash firmware to ESP32-C3 (with/without encryption)

Requires: esptool, pyserial
  pip install esptool pyserial
"""

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import threading
import subprocess
import sys
import os
import hmac
import hashlib
import time

# ─── same secret as src/main.cpp ───
LICENSE_SECRET = bytes([
    0x2A, 0x7C, 0xB9, 0x41, 0xD3, 0xE5, 0x8F, 0x06,
    0x9C, 0x15, 0xF7, 0xBA, 0x40, 0x62, 0x83, 0x1E
])

# VID:PID for known devices
ESP_VID_PIDS = {
    "esp32c3": "303A:1001",
    "esp8266": ["10C4:EA60", "1A86:7523", "1A86:55D4", "1A86:55D3", "0403:6001"],
}
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

def detect_chip_from_port(port):
    """Try to detect chip type from VID:PID. Returns 'esp32c3', 'esp8266', or None."""
    try:
        import serial.tools.list_ports
        for p in serial.tools.list_ports.comports():
            if p.device != port:
                continue
            if not (p.vid and p.pid):
                return None
            hwid = "{:04X}:{:04X}".format(p.vid, p.pid)
            if hwid == ESP_VID_PIDS["esp32c3"]:
                return "esp32c3"
            if hwid in ESP_VID_PIDS.get("esp8266", []):
                return "esp8266"
        return None
    except ImportError:
        return None


# ═══════════════════════════════════════════════════════════════
#  License functions
# ═══════════════════════════════════════════════════════════════

def generate_license_key(device_id: str) -> str:
    h = hmac.new(LICENSE_SECRET, device_id.upper().encode(), hashlib.sha256).digest()
    return h[:4].hex().upper()


# ═══════════════════════════════════════════════════════════════
#  Flash functions
# ═══════════════════════════════════════════════════════════════

def find_esp_port():
    """Return (device, chip_type) for first known ESP32-C3 or ESP8266, or (None, None)."""
    try:
        import serial.tools.list_ports
        ports = serial.tools.list_ports.comports()
        for p in ports:
            if p.vid and p.pid:
                hwid = "{:04X}:{:04X}".format(p.vid, p.pid)
                if hwid == ESP_VID_PIDS["esp32c3"]:
                    return p.device, "esp32c3"
                if hwid in ESP_VID_PIDS.get("esp8266", []):
                    return p.device, "esp8266"
    except ImportError:
        pass
    return None, None


def list_all_ports():
    """Return list of (device, description) for all serial ports."""
    result = []
    try:
        import serial.tools.list_ports
        ports = serial.tools.list_ports.comports()
        for p in ports:
            label = p.device
            if p.description and p.description != p.device:
                label += f" — {p.description}"
            if p.vid and p.pid:
                label += f"  [{p.vid:04X}:{p.pid:04X}]"
            result.append((p.device, label))
    except ImportError:
        pass
    return result


FLASH_LAYOUT = {
    "esp32c3": {
        "chip": "esp32c3",
        "firmware": "firmware_esp32c3.bin",
        "baud": "460800",
        "mode": "dio",
        "freq": "80m",
        "addrs": [("0x0000", "bootloader.bin"), ("0x8000", "partitions.bin"), ("0x10000", "firmware_esp32c3.bin")],
    },
    "esp8266": {
        "chip": "esp8266",
        "firmware": "firmware_esp8266.bin",
        "baud": "230400",
        "mode": "dout",
        "freq": "40m",
        "addrs": [("0x0000", "firmware_esp8266.bin")],
    },
}

def flash_firmware(port, chip_type="esp32c3", encrypt=False, keyfile=None, log_func=print):
    layout = FLASH_LAYOUT.get(chip_type, FLASH_LAYOUT["esp32c3"])
    python_exe = _find_python()

    cmd = [
        python_exe, "-m", "esptool",
        "--chip", layout["chip"],
        "--port", port,
        "--baud", layout["baud"],
        "write_flash", "-z",
        "--flash_mode", layout["mode"],
        "--flash_freq", layout["freq"],
        "--flash_size", "4MB",
    ]

    if encrypt:
        log_func("🔐 Flash Encryption ENABLED")
        cmd.append("--encrypt")
        if keyfile:
            cmd.extend(["--keyfile", keyfile])

    # Verify all files exist
    for addr, fname in layout["addrs"]:
        fpath = os.path.join(SCRIPT_DIR, fname)
        if not os.path.exists(fpath):
            log_func(f"❌ {fname} not found in {SCRIPT_DIR}!")
            return False

    # Add flash addresses and files
    for addr, fname in layout["addrs"]:
        cmd.extend([addr, os.path.join(SCRIPT_DIR, fname)])

    log_func(f"🚀 Flashing {port} ({chip_type}) …")
    for addr, fname in layout["addrs"]:
        log_func(f"   {fname} @ {addr}")
    if keyfile:
        log_func(f"   key file   : {keyfile}")

    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1)
        for line in iter(proc.stdout.readline, ''):
            if line:
                log_func(line.rstrip())
        proc.wait()
        if proc.returncode == 0:
            log_func("✅ Flash OK!")
            if encrypt:
                log_func("⚠  Flash Encryption eFuse BURNED — keep your key safe!")
            return True
        else:
            log_func(f"❌ Flash FAILED (code {proc.returncode})")
            return False
    except FileNotFoundError:
        log_func("❌ esptool not found. Run: pip install esptool")
        return False


def get_security_info(port, chip_type="esp32c3", log_func=print):
    python_exe = _find_python()
    baud = FLASH_LAYOUT.get(chip_type, FLASH_LAYOUT["esp32c3"])["baud"]
    cmd = [
        python_exe, "-m", "esptool",
        "--chip", chip_type,
        "--port", port,
        "--baud", baud,
        "get_security_info"
    ]
    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        for line in result.stdout.splitlines():
            log_func(line)
        return True
    except subprocess.CalledProcessError as e:
        log_func(f"❌ Security info failed: {e}")
        return False
    except FileNotFoundError:
        log_func("❌ esptool not installed")
        return False


# ═══════════════════════════════════════════════════════════════
#  GUI Application
# ═══════════════════════════════════════════════════════════════

class TpmsGui(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TPMS Emulator Tool v7.2")
        self.geometry("680x540")
        self.minsize(620, 480)
        self.configure(bg="#1a0533")

        # dark theme colors
        self.bg = "#1a0533"
        self.fg = "#e0e0e0"
        self.input_bg = "#0d1b3e"
        self.accent = "#7c4dff"
        self.success = "#69f0ae"
        self.error = "#ff5252"

        self.style = ttk.Style()
        self.style.theme_use("clam")
        self.style.configure("TNotebook", background=self.bg, borderwidth=0)
        self.style.configure("TNotebook.Tab",
                             background=self.input_bg, foreground=self.fg,
                             padding=[12, 6], font=("Segoe UI", 10))
        self.style.map("TNotebook.Tab",
                       background=[("selected", self.accent)],
                       foreground=[("selected", "#ffffff")])

        # ── notebook ──
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=True, padx=8, pady=8)

        # tabs
        self.tab_license = tk.Frame(self.notebook, bg=self.bg)
        self.tab_flash = tk.Frame(self.notebook, bg=self.bg)
        self.notebook.add(self.tab_license, text="  🔑 License  ")
        self.notebook.add(self.tab_flash, text="  ⚡ Flash  ")

        self._build_license_tab()
        self._build_flash_tab()

        # refresh COM ports every 3s
        self._refresh_ports()

    # ── helpers ──

    def _make_label(self, parent, text, **kw):
        return tk.Label(parent, text=text, bg=self.bg, fg=self.fg,
                        font=("Segoe UI", 10), anchor="w", **kw)

    def _make_entry(self, parent, **kw):
        e = tk.Entry(parent, bg=self.input_bg, fg=self.fg,
                     insertbackground=self.fg, relief="flat",
                     font=("Consolas", 11), **kw)
        return e

    def _make_btn(self, parent, text, cmd, color=None, **kw):
        bg = color or self.accent
        return tk.Button(parent, text=text, command=cmd,
                         bg=bg, fg="#ffffff", activebackground="#651fff",
                         activeforeground="#ffffff", relief="flat",
                         font=("Segoe UI", 10, "bold"), cursor="hand2",
                         padx=14, pady=6, bd=0, **kw)

    def _make_text_log(self, parent, height=10):
        t = tk.Text(parent, bg="#0a0a0a", fg="#9e9e9e",
                     insertbackground=self.fg, relief="flat",
                     font=("Consolas", 10), wrap="word", state="disabled",
                     height=height, bd=0)
        return t

    def _log_to(self, text_widget, msg):
        text_widget.configure(state="normal")
        text_widget.insert("end", msg + "\n")
        text_widget.see("end")
        text_widget.configure(state="disabled")

    # ── License Tab ──

    def _build_license_tab(self):
        f = self.tab_license

        # header
        tk.Label(f, text="🔑 License Key Generator",
                 bg=self.bg, fg=self.fg,
                 font=("Segoe UI", 14, "bold")).pack(anchor="w", pady=(10, 4))
        tk.Label(f, text="HMAC-SHA256 · первый 4 байта → 8 hex символов",
                 bg=self.bg, fg="#9e9e9e",
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(0, 12))

        # device id
        row1 = tk.Frame(f, bg=self.bg)
        row1.pack(fill="x", pady=4)
        self._make_label(row1, "Device ID (12 hex символов):").pack(side="left")
        self.entry_devid = self._make_entry(row1)
        self.entry_devid.pack(side="left", fill="x", expand=True, padx=(8, 0))
        self._add_context_menu(self.entry_devid)

        # key output
        row2 = tk.Frame(f, bg=self.bg)
        row2.pack(fill="x", pady=4)
        self._make_label(row2, "License Key:").pack(side="left")

        # Use normal state (not readonly) so text can be selected/copied
        self.entry_key = self._make_entry(row2, state="normal")
        self.entry_key.configure(fg="#69f0ae")  # distinct color for generated key
        self.entry_key.pack(side="left", fill="x", expand=True, padx=(8, 0))
        self._add_context_menu(self.entry_key)

        # buttons row
        row3 = tk.Frame(f, bg=self.bg)
        row3.pack(fill="x", pady=(8, 2))
        self._make_btn(row3, "  🔑 Generate  ", self._on_generate).pack(side="left", padx=(0, 6))
        self._make_btn(row3, "  📋 Copy Key  ", self._on_copy).pack(side="left", padx=6)
        self._make_btn(row3, "  🔄 Paste ID from Clipboard  ", self._on_paste_id,
                       color="#00c853").pack(side="left", padx=6)

        # hint
        tk.Label(f, text="Device ID берётся из карточки «Лицензия» в веб-интерфейсе устройства",
                 bg=self.bg, fg="#555", font=("Segoe UI", 8)).pack(anchor="w", pady=(2, 0))

        # separator + flash integration hint
        tk.Frame(f, height=1, bg="#333").pack(fill="x", pady=(16, 8))
        tk.Label(f, text="После генерации ключа перейдите на вкладку ⚡ Flash для прошивки",
                 bg=self.bg, fg="#555", font=("Segoe UI", 9)).pack(anchor="w")

    def _on_generate(self):
        did = self.entry_devid.get().strip().upper()
        if len(did) != 12 or not all(c in "0123456789ABCDEF" for c in did):
            messagebox.showerror("Error", "Device ID должен быть 12 hex символов (0-9, A-F)")
            return
        key = generate_license_key(did)
        self.entry_key.delete(0, "end")
        self.entry_key.insert(0, key)

    def _on_copy(self):
        key = self.entry_key.get()
        if key:
            self.clipboard_clear()
            self.clipboard_append(key)
            self._flash_status("📋 License key copied!")

    def _on_paste_id(self):
        try:
            txt = self.clipboard_get().strip()
            if len(txt) >= 12:
                self.entry_devid.delete(0, "end")
                self.entry_devid.insert(0, txt[:12].upper())
        except:
            pass

    # ── Flash Tab ──

    def _build_flash_tab(self):
        f = self.tab_flash

        # header
        tk.Label(f, text="⚡ Flash Firmware",
                 bg=self.bg, fg=self.fg,
                 font=("Segoe UI", 14, "bold")).pack(anchor="w", pady=(10, 4))
        tk.Label(f, text="ESP32-C3 или ESP8266 · esptool · двойная платформа",
                 bg=self.bg, fg="#9e9e9e",
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(0, 12))

        # COM port row
        row1 = tk.Frame(f, bg=self.bg)
        row1.pack(fill="x", pady=4)
        self._make_label(row1, "COM Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(row1, textvariable=self.port_var,
                                        state="readonly", width=50,
                                        font=("Consolas", 10))
        self.port_combo.pack(side="left", fill="x", expand=True, padx=(8, 0))
        self._make_btn(row1, "  🔄  ", self._refresh_ports,
                       color="#555").pack(side="left", padx=(6, 0))

        # Platform row
        row1b = tk.Frame(f, bg=self.bg)
        row1b.pack(fill="x", pady=4)
        self._make_label(row1b, "Platform:").pack(side="left")
        self.platform_var = tk.StringVar(value="esp32c3")
        self.platform_combo = ttk.Combobox(row1b, textvariable=self.platform_var,
                                            values=["esp32c3", "esp8266"],
                                            state="readonly", width=20,
                                            font=("Consolas", 10))
        self.platform_combo.pack(side="left", padx=(8, 0))

        # encryption key row
        row2 = tk.Frame(f, bg=self.bg)
        row2.pack(fill="x", pady=4)
        self._make_label(row2, "Encryption Key:").pack(side="left")
        self.entry_keyfile = self._make_entry(row2)
        self.entry_keyfile.pack(side="left", fill="x", expand=True, padx=(8, 0))
        self._make_btn(row2, "  📂  ", self._on_browse_key,
                       color="#555").pack(side="left", padx=(6, 0))

        # checkboxes
        row3 = tk.Frame(f, bg=self.bg)
        row3.pack(fill="x", pady=(8, 4))
        self.encrypt_var = tk.BooleanVar()
        cb = tk.Checkbutton(row3, text="🔐 Flash Encryption (прожигает eFuse!)",
                            variable=self.encrypt_var, bg=self.bg, fg=self.error,
                            selectcolor=self.input_bg, font=("Segoe UI", 10, "bold"),
                            activebackground=self.bg, activeforeground=self.error)
        cb.pack(side="left")

        # buttons
        row4 = tk.Frame(f, bg=self.bg)
        row4.pack(fill="x", pady=(6, 4))
        self.btn_flash = self._make_btn(row4, "  ⚡ Flash  ", self._on_flash,
                                        color=self.accent)
        self.btn_flash.pack(side="left", padx=(0, 6))
        self.btn_encrypt = self._make_btn(row4, "  🔒 Flash + Encrypt  ",
                                          self._on_flash_encrypt, color="#ff5252")
        self.btn_encrypt.pack(side="left", padx=6)
        self.btn_info = self._make_btn(row4, "  ℹ Security Info  ",
                                       self._on_security_info, color="#555")
        self.btn_info.pack(side="left", padx=6)

        # log
        tk.Label(f, text="Log:", bg=self.bg, fg="#9e9e9e",
                 font=("Segoe UI", 9)).pack(anchor="w", pady=(8, 2))
        self.log_text = self._make_text_log(f, height=12)
        self.log_text.pack(fill="both", expand=True)

    def _refresh_ports(self):
        ports = list_all_ports()
        values = [label for _, label in ports]
        self.port_combo["values"] = values if values else ["(no ports found)"]

        # try to auto-select known ESP
        auto_port, chip = find_esp_port()
        if auto_port and chip:
            self.platform_var.set(chip)
            for dev, label in ports:
                if dev == auto_port:
                    self.port_var.set(label)
                    return
        if ports:
            self.port_var.set(values[0])

    def _on_browse_key(self):
        path = filedialog.askopenfilename(
            title="Select Flash Encryption Key",
            filetypes=[("Binary key", "*.bin"), ("All files", "*.*")],
            initialdir=SCRIPT_DIR
        )
        if path:
            self.entry_keyfile.delete(0, "end")
            self.entry_keyfile.insert(0, path)

    def _on_flash(self):
        self._do_flash(encrypt=False)

    def _on_flash_encrypt(self):
        if not self.encrypt_var.get():
            if not messagebox.askyesno(
                    "⚠ Flash Encryption",
                    "Это ПЕРВЫЙ запуск с шифрованием?\n"
                    "Flash Encryption ПРОЖИГАЕТ eFuse — процедура НЕОБРАТИМА.\n\n"
                    "После этого чип принимает ТОЛЬКО зашифрованную прошивку.\n\n"
                    "Продолжить?",
                    icon="warning"):
                return
        self._do_flash(encrypt=True)

    def _do_flash(self, encrypt=False):
        port_label = self.port_var.get()
        if not port_label or port_label.startswith("(no"):
            messagebox.showerror("Error", "Устройство не найдено. Подключите устройство.")
            return

        # extract device name from label
        port = port_label.split(" —")[0].split("  [")[0].strip()
        chip_type = self.platform_var.get()

        keyfile = self.entry_keyfile.get().strip() or None
        if encrypt and keyfile and not os.path.exists(keyfile):
            messagebox.showerror("Error", f"Encryption key file not found:\n{keyfile}")
            return

        # disable buttons during flash
        for b in (self.btn_flash, self.btn_encrypt, self.btn_info):
            b.configure(state="disabled")

        self._log_to(self.log_text, f"\n{'=' * 50}")
        self._log_to(self.log_text, f"  Flashing {port} ({chip_type}) …")
        self._log_to(self.log_text, f"{'=' * 50}")

        def worker():
            try:
                flash_firmware(port, chip_type=chip_type, encrypt=encrypt, keyfile=keyfile,
                               log_func=lambda m: self.after(0, self._log_to, self.log_text, m))
            finally:
                self.after(0, lambda: [b.configure(state="normal")
                                       for b in (self.btn_flash, self.btn_encrypt, self.btn_info)])

        threading.Thread(target=worker, daemon=True).start()

    def _on_security_info(self):
        port_label = self.port_var.get()
        if not port_label or port_label.startswith("(no"):
            messagebox.showerror("Error", "Устройство не найдено.")
            return
        port = port_label.split(" —")[0].split("  [")[0].strip()
        chip_type = self.platform_var.get()

        self._log_to(self.log_text, "\n--- Security Info ---")

        def worker():
            get_security_info(port, chip_type=chip_type,
                              log_func=lambda m: self.after(0, self._log_to, self.log_text, m))

        threading.Thread(target=worker, daemon=True).start()

    def _flash_status(self, msg):
        self._log_to(self.log_text, msg)

    # ── context menu for copy/paste ──

    def _add_context_menu(self, widget):
        """Add right-click context menu (Copy/Paste) + keyboard shortcuts to an Entry."""
        menu = tk.Menu(self, tearoff=0, bg=self.input_bg, fg=self.fg,
                       activebackground=self.accent, activeforeground="#fff")
        menu.add_command(label="Копировать", command=lambda: self._ctx_copy(widget))
        menu.add_command(label="Вставить", command=lambda: self._ctx_paste(widget))
        menu.add_separator()
        menu.add_command(label="Выделить всё", command=lambda: self._ctx_select_all(widget))

        def show_menu(event):
            try:
                menu.tk_popup(event.x_root, event.y_root)
            finally:
                menu.grab_release()

        widget.bind("<Button-3>", show_menu)       # right-click (Windows)
        widget.bind("<Button-2>", show_menu)       # right-click (mac)
        widget.bind("<Control-c>", lambda e: self._ctx_copy(widget))
        widget.bind("<Control-C>", lambda e: self._ctx_copy(widget))
        widget.bind("<Control-v>", lambda e: self._ctx_paste(widget))
        widget.bind("<Control-V>", lambda e: self._ctx_paste(widget))
        widget.bind("<Control-a>", lambda e: self._ctx_select_all(widget))
        widget.bind("<Control-A>", lambda e: self._ctx_select_all(widget))

    def _ctx_copy(self, widget):
        try:
            sel = widget.selection_get()
            self.clipboard_clear()
            self.clipboard_append(sel)
        except tk.TclError:
            pass

    def _ctx_paste(self, widget):
        try:
            txt = self.clipboard_get()
            widget.insert("insert", txt)
        except tk.TclError:
            pass

    def _ctx_select_all(self, widget):
        widget.selection_range(0, "end")
        widget.icursor("end")


# ═══════════════════════════════════════════════════════════════

if __name__ == "__main__":
    app = TpmsGui()
    app.mainloop()
