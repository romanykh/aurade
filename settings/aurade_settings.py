#!/usr/bin/env python3
"""
AuraDE Settings — the complete control panel for the AuraDE desktop environment.

Pages:
  - Wi-Fi: wireless scan, connect, disconnect, password prompt
  - Bluetooth: adapter power, discoverable, paired devices, connect/disconnect/remove, scan
  - Sound: output/input volume, mute, device selection, sound test
  - Displays: brightness, resolution modes, night light, scaling
  - Mouse & Touchpad: tap-to-click, speed, natural scrolling, acceleration profile
  - Keyboard: layout, variants, key repeat delay/interval, shortcut reference
  - Appearance: dark mode, accent colour, cursor theme & size, desktop wallpaper
  - Dock: autohide, intellihide, icon size, magnification
  - Gestures: 3 & 4-finger gestures, physics tuning, live telemetry
  - About: system hardware/OS info, configuration management
"""

import glob
import json
import os
import platform
import re
import socket
import subprocess
import sys
import threading
import time

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gtk, Gdk, GLib, Pango

CONFIG_PATH = os.path.expanduser("~/.config/aurade/config.json")
SOCKET_PATH = "/tmp/aurade-gesture.sock"

# Mirrors config_get_defaults() in src/config/config.c
DEFAULTS = {
    "gestures": {
        "swipe_up_3": "mission_control",
        "swipe_down_3": "app_expose",
        "swipe_left_3": "workspace_next",
        "swipe_right_3": "workspace_prev",
        "pinch_in_3": "none",
        "pinch_out_3": "show_desktop",
        "swipe_up_4": "mission_control",
        "swipe_down_4": "app_expose",
        "swipe_left_4": "workspace_next",
        "swipe_right_4": "workspace_prev",
        "pinch_in_4": "launchpad",
        "pinch_out_4": "launchpad",
        "edge_swipe_right": "control_center",
    },
    "physics": {
        "stiffness": 280.0,
        "damping": 0.92,
        "mass": 1.0,
        "sensitivity": 1.0,
        "snap_threshold": 0.35,
        "flick_velocity_threshold": 700.0,
        "rubber_band_limit": 180.0,
        "deadzone_px": 40.0,
        "natural_scroll": True,
        "disable_while_typing": False,
    },
    "keyboard": {"layout": "", "variant": "", "model": "", "options": ""},
    "dock": {
        "enabled": True,
        "autohide": False,
        "intellihide": True,
        "icon_size": 52,
        "magnification": True,
        "max_scale": 1.5,
        "mag_radius": 130.0,
        "theme": "modern",
        "opacity": 0.85,
        "pinned": [
            {"name": "Launchpad", "exec": ""},
        ],
    },
    "appearance": {
        "dark_mode": True,
        "wallpaper_path": "",
        "accent_color": "#007AFF",
        "window_opacity": 1.0,
    },
    "workspaces": {
        "count": 4,
        "auto_focus": True,
        "tiling": False,
        "tiling_gap": 10,
    },
    "blur": {
        "dock": True,
        "topbar": True,
        "launchpad": True,
        "workspace_overview": True,
        "windows": True,
        "strength": 1.0,
    },
    "music_pill": {
        "enabled": True,
        "position": "topbar_center",
    },
}

ACTIONS = [
    ("mission_control", "Mission Control"),
    ("app_expose", "App Exposé"),
    ("workspace_next", "Next Workspace"),
    ("workspace_prev", "Previous Workspace"),
    ("launchpad", "Launchpad"),
    ("show_desktop", "Show Desktop"),
    ("control_center", "Control Center"),
    ("none", "Disabled"),
]
ACTION_VALUES = [a for a, _ in ACTIONS]
ACTION_LABELS = [label for _, label in ACTIONS]
ACTION_INDEX = {value: i for i, (value, _) in enumerate(ACTIONS)}

PAGES = [
    ("wifi", "Wi-Fi", "network-wireless-symbolic", "badge-wifi"),
    ("bluetooth", "Bluetooth", "bluetooth-symbolic", "badge-bluetooth"),
    ("sound", "Sound", "audio-volume-high-symbolic", "badge-sound"),
    ("display", "Displays", "video-display-symbolic", "badge-display"),
    ("workspaces", "Workspaces", "view-grid-symbolic", "badge-workspaces"),
    ("mouse", "Mouse & Touchpad", "input-mouse-symbolic", "badge-mouse"),
    ("keyboard", "Keyboard", "input-keyboard-symbolic", "badge-keyboard"),
    ("appearance", "Appearance", "preferences-desktop-wallpaper-symbolic", "badge-appearance"),
    ("dock", "Dock", "view-paged-symbolic", "badge-dock"),
    ("gestures", "Gestures", "input-touchpad-symbolic", "badge-gestures"),
    ("about", "About", "help-about-symbolic", "badge-about"),
]

GESTURE_GROUPS = [
    ("Three fingers", [
        ("swipe_up_3", "Swipe up"),
        ("swipe_down_3", "Swipe down"),
        ("swipe_left_3", "Swipe left"),
        ("swipe_right_3", "Swipe right"),
        ("pinch_in_3", "Pinch in"),
        ("pinch_out_3", "Pinch out"),
    ]),
    ("Four fingers", [
        ("swipe_up_4", "Swipe up"),
        ("swipe_down_4", "Swipe down"),
        ("swipe_left_4", "Swipe left"),
        ("swipe_right_4", "Swipe right"),
        ("pinch_in_4", "Pinch in"),
        ("pinch_out_4", "Pinch out"),
    ]),
]

SLIDERS = [
    ("sensitivity", "Sensitivity", "Higher values need less finger travel for the same action.",
     0.4, 3.0, 0.05, 2),
    ("snap_threshold", "Snap threshold", "How far a slow drag must go before it completes the gesture.",
     0.15, 0.65, 0.01, 2),
    ("deadzone_px", "Recognition slop", "Movement ignored before a gesture starts. ~19 px per mm; 80 px ≈ 4 mm.",
     16.0, 160.0, 2.0, 0),
    ("flick_velocity_threshold", "Flick speed", "Release speed that counts as a flick and completes the gesture.",
     300.0, 4000.0, 50.0, 0),
    ("stiffness", "Spring stiffness", "Stiffness of the settle animation.",
     80.0, 800.0, 10.0, 0),
    ("damping", "Spring damping", "Below 1.0 bounces, above 1.0 feels sluggish.",
     0.40, 1.30, 0.01, 2),
    ("mass", "Spring mass", "Heavier feels slower and more ponderous.",
     0.5, 3.0, 0.1, 1),
    ("rubber_band_limit", "Rubber-band limit", "How far the carousel stretches past the first/last workspace.",
     60.0, 500.0, 10.0, 0),
]

def build_css(dark: bool) -> str:
    if dark:
        win_bg = "#1e1e20"
        sidebar_bg = "rgba(40, 40, 44, 0.95)"
        sidebar_border = "rgba(255, 255, 255, 0.08)"
        card_bg = "rgba(255, 255, 255, 0.055)"
        card_border = "rgba(255, 255, 255, 0.09)"
        row_sep = "rgba(255, 255, 255, 0.07)"
        text_primary = "#f5f5f7"
        text_secondary = "rgba(235, 235, 245, 0.60)"
        text_tertiary = "rgba(235, 235, 245, 0.38)"
        hover_bg = "rgba(255, 255, 255, 0.06)"
        btn_sec_bg = "rgba(255, 255, 255, 0.10)"
        btn_sec_hover = "rgba(255, 255, 255, 0.15)"
        entry_bg = "rgba(0, 0, 0, 0.28)"
        header_bg = "#252528"
    else:
        win_bg = "#f5f5f7"
        sidebar_bg = "rgba(232, 232, 237, 0.95)"
        sidebar_border = "rgba(0, 0, 0, 0.10)"
        card_bg = "#ffffff"
        card_border = "rgba(0, 0, 0, 0.09)"
        row_sep = "rgba(0, 0, 0, 0.07)"
        text_primary = "#1d1d1f"
        text_secondary = "rgba(60, 60, 67, 0.68)"
        text_tertiary = "rgba(60, 60, 67, 0.45)"
        hover_bg = "rgba(0, 0, 0, 0.05)"
        btn_sec_bg = "rgba(0, 0, 0, 0.06)"
        btn_sec_hover = "rgba(0, 0, 0, 0.10)"
        entry_bg = "rgba(0, 0, 0, 0.04)"
        header_bg = "#ececee"

    return f"""
    window, window.csd, window.background {{
        background-color: {win_bg};
        color: {text_primary};
        font-family: "SF Pro Text", "Inter", -apple-system, BlinkMacSystemFont, sans-serif;
    }}

    headerbar {{
        background-color: {header_bg};
        color: {text_primary};
        border-bottom: 1px solid {sidebar_border};
        min-height: 44px;
        padding: 0 12px;
        box-shadow: none;
    }}

    headerbar .header-title {{
        font-size: 14px;
        font-weight: 700;
        color: {text_primary};
    }}

    /* Inset macOS Sonoma Sidebar */
    .sidebar {{
        background-color: {sidebar_bg};
        border: 1px solid {sidebar_border};
        border-radius: 14px;
        margin-top: 12px;
        margin-bottom: 12px;
        margin-left: 12px;
        padding: 10px 8px;
    }}

    .profile-card {{
        background-color: {card_bg};
        border: 1px solid {card_border};
        border-radius: 11px;
        padding: 10px 12px;
        margin: 2px 4px 10px 4px;
    }}

    .profile-avatar {{
        min-width: 38px;
        min-height: 38px;
        border-radius: 19px;
        background: linear-gradient(145deg, #0A84FF, #5E5CE6);
    }}

    .profile-avatar image {{
        color: #ffffff;
        -gtk-icon-size: 20px;
        margin: 9px;
    }}

    .profile-name {{
        font-size: 13.5px;
        font-weight: 700;
        color: {text_primary};
    }}

    .profile-sub {{
        font-size: 11.5px;
        color: {text_secondary};
    }}

    .sidebar-section-label {{
        font-size: 11px;
        font-weight: 700;
        letter-spacing: 0.5px;
        color: {text_tertiary};
        margin-left: 10px;
        margin-top: 4px;
        margin-bottom: 4px;
    }}

    .sidebar list {{
        background: transparent;
    }}

    .sidebar row {{
        border-radius: 8px;
        padding: 3px 6px;
        margin: 1px 2px;
        color: {text_primary};
        font-size: 13px;
        font-weight: 500;
    }}

    .sidebar row:hover {{
        background-color: {hover_bg};
    }}

    .sidebar row:selected {{
        background-color: #007AFF;
        color: #ffffff;
        font-weight: 600;
    }}

    .sidebar row:selected label {{
        color: #ffffff;
    }}

    /* Strictly fixed 26x26 macOS Squircle Sidebar Icon Badges */
    .sidebar-badge {{
        min-width: 26px;
        min-height: 26px;
        border-radius: 7px;
        margin-right: 8px;
        box-shadow: 0 1px 2px rgba(0, 0, 0, 0.22), inset 0 1px 0 rgba(255, 255, 255, 0.25);
    }}

    .sidebar-badge image {{
        color: #ffffff;
        -gtk-icon-size: 15px;
        margin-left: 5px;
        margin-right: 5px;
        margin-top: 5px;
        margin-bottom: 5px;
    }}

    .row-badge {{
        min-width: 28px;
        min-height: 28px;
        border-radius: 7px;
        box-shadow: 0 1px 2px rgba(0, 0, 0, 0.20), inset 0 1px 0 rgba(255, 255, 255, 0.22);
    }}

    .row-badge image {{
        color: #ffffff;
        -gtk-icon-size: 15px;
        margin: 6px;
    }}

    .badge-wifi       {{ background: linear-gradient(180deg, #3498ff 0%, #007AFF 100%); }}
    .badge-bluetooth  {{ background: linear-gradient(180deg, #2E9AFE 0%, #0A66C2 100%); }}
    .badge-sound      {{ background: linear-gradient(180deg, #FF6259 0%, #FF3B30 100%); }}
    .badge-display    {{ background: linear-gradient(180deg, #5AC8FA 0%, #007AFF 100%); }}
    .badge-workspaces {{ background: linear-gradient(180deg, #7D7AFF 0%, #5856D6 100%); }}
    .badge-mouse      {{ background: linear-gradient(180deg, #FF5E7E 0%, #FF2D55 100%); }}
    .badge-keyboard   {{ background: linear-gradient(180deg, #98989D 0%, #636366 100%); }}
    .badge-appearance {{ background: linear-gradient(180deg, #BF5AF2 0%, #8944AB 100%); }}
    .badge-dock       {{ background: linear-gradient(180deg, #FFB340 0%, #FF9500 100%); }}
    .badge-gestures   {{ background: linear-gradient(180deg, #4CD964 0%, #28CD41 100%); }}
    .badge-about      {{ background: linear-gradient(180deg, #8E8E93 0%, #48484A 100%); }}

    .sidebar-footer {{
        background-color: {card_bg};
        border: 1px solid {card_border};
        border-radius: 10px;
        padding: 8px 10px;
        margin: 6px 4px 2px 4px;
    }}

    /* Grouped macOS Sonoma Cards & Rows */
    .headline {{
        font-size: 21px;
        font-weight: 700;
        letter-spacing: -0.3px;
        color: {text_primary};
    }}

    .lead {{
        font-size: 12.5px;
        color: {text_secondary};
        margin-top: 3px;
        margin-bottom: 14px;
    }}

    .section {{
        font-size: 11.5px;
        font-weight: 700;
        letter-spacing: 0.4px;
        color: {text_secondary};
        margin: 14px 4px 6px 4px;
    }}

    .card {{
        background-color: {card_bg};
        border: 1px solid {card_border};
        border-radius: 12px;
        padding: 2px 0;
        margin-bottom: 6px;
    }}

    .row {{
        padding: 11px 16px;
    }}

    .mac-sep {{
        background-color: {row_sep};
        min-height: 1px;
        margin-left: 16px;
        margin-right: 16px;
    }}

    .title {{
        font-weight: 600;
        font-size: 13.5px;
        color: {text_primary};
    }}

    .subtitle {{
        font-size: 12px;
        color: {text_secondary};
    }}

    .mono {{
        font-family: "JetBrains Mono", monospace;
        font-size: 12px;
        color: {text_secondary};
        background-color: {btn_sec_bg};
        border-radius: 6px;
        padding: 3px 8px;
    }}

    .hero-banner {{
        background: linear-gradient(135deg, rgba(10, 132, 255, 0.16), rgba(94, 92, 230, 0.14));
        border: 1px solid rgba(10, 132, 255, 0.30);
        border-radius: 14px;
        padding: 16px 20px;
        margin-bottom: 6px;
    }}

    .connected-pill {{
        background-color: rgba(48, 209, 88, 0.16);
        color: #30D158;
        border: 1px solid rgba(48, 209, 88, 0.32);
        border-radius: 999px;
        padding: 3px 10px;
        font-size: 11.5px;
        font-weight: 700;
    }}

    .disconnected-pill {{
        background-color: {btn_sec_bg};
        color: {text_secondary};
        border: 1px solid {card_border};
        border-radius: 999px;
        padding: 3px 10px;
        font-size: 11.5px;
        font-weight: 600;
    }}

    scrolledwindow button, .header-btn {{
        background-color: {btn_sec_bg};
        color: {text_primary};
        border: 1px solid {card_border};
        border-radius: 8px;
        padding: 5px 12px;
        font-size: 12.5px;
        font-weight: 600;
    }}

    scrolledwindow button:hover, .header-btn:hover {{
        background-color: {btn_sec_hover};
    }}

    .btn-primary {{
        background: linear-gradient(180deg, #1A89FF 0%, #007AFF 100%);
        color: #ffffff;
        font-weight: 600;
        border-radius: 8px;
        padding: 5px 14px;
        border: none;
        box-shadow: 0 1px 2px rgba(0, 0, 0, 0.20);
    }}

    .btn-primary:hover {{
        background: linear-gradient(180deg, #3396FF 0%, #0A84FF 100%);
    }}

    .btn-destructive {{
        background-color: rgba(255, 69, 58, 0.15);
        color: #FF453A;
        border: 1px solid rgba(255, 69, 58, 0.25);
        font-weight: 600;
        border-radius: 8px;
        padding: 5px 12px;
    }}

    .btn-destructive:hover {{
        background-color: rgba(255, 69, 58, 0.24);
    }}

    entry, passwordentry {{
        background-color: {entry_bg};
        color: {text_primary};
        border: 1px solid {card_border};
        border-radius: 8px;
        padding: 5px 10px;
        font-size: 13px;
    }}

    .workspace-card {{
        background-color: {card_bg};
        border: 1px solid {card_border};
        border-radius: 12px;
        padding: 12px 14px;
        min-width: 130px;
    }}

    .workspace-card-add {{
        border: 1.5px dashed rgba(10, 132, 255, 0.45);
        border-radius: 12px;
        padding: 12px 14px;
        min-width: 120px;
    }}
    """


def make_badge(icon_name: str, color_class: str, size_class: str = "sidebar-badge") -> Gtk.Box:
    """Create a strictly fixed-size macOS squircle icon badge that never stretches."""
    badge = Gtk.Box()
    badge.add_css_class(size_class)
    badge.add_css_class(color_class)
    badge.set_halign(Gtk.Align.CENTER)
    badge.set_valign(Gtk.Align.CENTER)
    badge.set_hexpand(False)
    badge.set_vexpand(False)

    icon = Gtk.Image.new_from_icon_name(icon_name)
    icon.set_halign(Gtk.Align.CENTER)
    icon.set_valign(Gtk.Align.CENTER)
    icon.set_hexpand(False)
    icon.set_vexpand(False)
    badge.append(icon)
    return badge


# ---------------------------------------------------------------------------
# Config and GSettings Helpers
# ---------------------------------------------------------------------------

def merge_defaults(cfg, defaults):
    if not isinstance(cfg, dict):
        cfg = {}
    for key, value in defaults.items():
        if isinstance(value, dict):
            cfg[key] = merge_defaults(cfg.get(key, {}), value)
        elif key not in cfg:
            cfg[key] = value
    return cfg


def load_config():
    raw = {}
    try:
        with open(CONFIG_PATH, "r") as f:
            raw = json.load(f)
    except FileNotFoundError:
        pass
    except Exception as exc:
        print("AuraDE settings: could not read config:", exc)
    merged = merge_defaults(raw, DEFAULTS)
    # Ensure tiling_gap has a sane default (10) if set to 0.0 or missing
    if "workspaces" in merged:
        gap = merged["workspaces"].get("tiling_gap", 10)
        if gap is None or gap <= 0:
            merged["workspaces"]["tiling_gap"] = 10
    return merged


def gsettings_get(schema, key, default=None):
    try:
        res = subprocess.run(["gsettings", "get", schema, key], capture_output=True, text=True, timeout=2)
        if res.returncode == 0:
            out = res.stdout.strip()
            if out.startswith("'") and out.endswith("'"):
                return out[1:-1]
            if out.lower() == "true":
                return True
            if out.lower() == "false":
                return False
            if out.startswith("uint32 "):
                return int(out.split()[1])
            try:
                return float(out)
            except ValueError:
                return out
    except Exception:
        pass
    return default


def gsettings_set(schema, key, value):
    try:
        if isinstance(value, bool):
            val_str = "true" if value else "false"
        elif isinstance(value, (int, float)):
            val_str = str(value)
        else:
            val_str = f"'{value}'"
        subprocess.run(["gsettings", "set", schema, key, val_str], capture_output=True, timeout=2)
    except Exception as e:
        print("gsettings_set error:", e)


def hex_rgba(value):
    rgba = Gdk.RGBA()
    try:
        rgba.parse(value)
    except Exception:
        rgba.parse("#007AFF")
    return rgba


def rgba_hex(rgba):
    return "#{:02X}{:02X}{:02X}".format(
        round(rgba.red * 255), round(rgba.green * 255), round(rgba.blue * 255))


# ---------------------------------------------------------------------------
# Live gesture telemetry
# ---------------------------------------------------------------------------

class Telemetry(threading.Thread):
    def __init__(self, callback):
        super().__init__(daemon=True)
        self.callback = callback
        self.running = True

    def run(self):
        while self.running:
            if not os.path.exists(SOCKET_PATH):
                time.sleep(0.6)
                continue
            sock = None
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.connect(SOCKET_PATH)
                buffer = ""
                while self.running:
                    data = sock.recv(4096)
                    if not data:
                        break
                    buffer += data.decode("utf-8", errors="ignore")
                    while "\n" in buffer:
                        line, buffer = buffer.split("\n", 1)
                        if line.strip():
                            try:
                                GLib.idle_add(self.callback, json.loads(line))
                            except Exception:
                                pass
            except Exception:
                time.sleep(0.6)
            finally:
                if sock is not None:
                    try:
                        sock.close()
                    except Exception:
                        pass


# ---------------------------------------------------------------------------
# Main Settings Application
# ---------------------------------------------------------------------------

class AuraSettings(Gtk.Application):
    def __init__(self):
        super().__init__(application_id="ai.user.aurade.settings")
        self.cfg = load_config()
        self.win = None
        self.loading = True
        self.save_source = 0
        self.status = None
        self.telemetry = None
        self.css_provider = Gtk.CssProvider()

        page_names = [name for name, _, _, _ in PAGES]
        self.initial_page = os.environ.get("AURADE_SETTINGS_PAGE", "wifi")
        if len(sys.argv) > 1 and sys.argv[1] in page_names:
            self.initial_page = sys.argv[1]
        elif self.initial_page not in page_names:
            self.initial_page = "wifi"

    # -- Lifecycle ---------------------------------------------------------

    def do_activate(self):
        if self.win:
            self.win.present()
            return

        self.win = Gtk.ApplicationWindow(application=self, title="System Settings")
        self.win.set_default_size(880, 600)
        self.win.set_size_request(460, 360)
        self.win.connect("close-request", self.on_close)

        header = Gtk.HeaderBar()
        header.set_show_title_buttons(True)
        self.header_title_lbl = Gtk.Label(label="System Settings")
        self.header_title_lbl.add_css_class("header-title")
        header.set_title_widget(self.header_title_lbl)

        # Quick Aura Connect launcher in HeaderBar
        connect_btn = Gtk.Button(label="Aura Connect…")
        connect_btn.add_css_class("header-btn")
        connect_btn.set_valign(Gtk.Align.CENTER)
        connect_btn.connect(
            "clicked",
            lambda *_: subprocess.Popen(
                [sys.executable, "/home/user/AI/aura-connect/linux/aura_connect_app.py"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
        )
        header.pack_start(connect_btn)

        self.status = Gtk.Label(label="Ready")
        self.status.add_css_class("disconnected-pill")
        self.status.set_valign(Gtk.Align.CENTER)
        header.pack_end(self.status)
        self.win.set_titlebar(header)

        self.build_ui()
        self.loading = False
        self.start_telemetry()
        self.win.present()

    def build_ui(self):
        root = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        self.win.set_child(root)

        self.stack = Gtk.Stack()
        self.stack.set_hexpand(True)
        self.stack.set_vexpand(True)
        self.stack.set_transition_type(Gtk.StackTransitionType.CROSSFADE)
        self.stack.set_transition_duration(150)

        root.append(self.build_sidebar())
        root.append(self.stack)

        # Build all pages
        self.stack.add_named(self.scrolled(self.page_wifi()), "wifi")
        self.stack.add_named(self.scrolled(self.page_bluetooth()), "bluetooth")
        self.stack.add_named(self.scrolled(self.page_sound()), "sound")
        self.stack.add_named(self.scrolled(self.page_display()), "display")
        self.stack.add_named(self.scrolled(self.page_workspaces()), "workspaces")
        self.stack.add_named(self.scrolled(self.page_mouse()), "mouse")
        self.stack.add_named(self.scrolled(self.page_keyboard()), "keyboard")
        self.stack.add_named(self.scrolled(self.page_appearance()), "appearance")
        self.stack.add_named(self.scrolled(self.page_dock()), "dock")
        self.stack.add_named(self.scrolled(self.page_gestures()), "gestures")
        self.stack.add_named(self.scrolled(self.page_about()), "about")

        page_names = [name for name, _, _, _ in PAGES]
        index = page_names.index(self.initial_page) if self.initial_page in page_names else 0
        self.sidebar_list.select_row(self.sidebar_list.get_row_at_index(index))
        self.stack.set_visible_child_name(self.initial_page)

        self.apply_theme_class()

    def on_close(self, *_args):
        if hasattr(self, "_stop_bt_scan"):
            self._stop_bt_scan()
        self.save_now()
        self.win = None
        return False

    def apply_theme_class(self):
        dark = bool(self.cfg["appearance"].get("dark_mode", True))
        self.css_provider.load_from_data(build_css(dark).encode("utf-8"))
        display = Gdk.Display.get_default()
        if display:
            Gtk.StyleContext.add_provider_for_display(
                display, self.css_provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
            )

    # -- Generic Widgets ---------------------------------------------------

    def scrolled(self, child):
        sw = Gtk.ScrolledWindow()
        sw.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        sw.set_child(child)
        return sw

    def build_sidebar(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        box.set_size_request(200, -1)
        box.set_hexpand(False)
        box.set_vexpand(True)
        box.add_css_class("sidebar")

        # Top macOS Apple-ID style User / Host Profile Card
        prof = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        prof.add_css_class("profile-card")
        prof.set_vexpand(False)

        avatar = Gtk.Box()
        avatar.add_css_class("profile-avatar")
        avatar.set_valign(Gtk.Align.CENTER)
        avatar.set_halign(Gtk.Align.CENTER)
        avatar.set_hexpand(False)
        avatar.set_vexpand(False)
        av_ic = Gtk.Image.new_from_icon_name("avatar-default-symbolic")
        av_ic.set_halign(Gtk.Align.CENTER)
        av_ic.set_valign(Gtk.Align.CENTER)
        avatar.append(av_ic)
        prof.append(avatar)

        pcol = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=1)
        pcol.set_valign(Gtk.Align.CENTER)
        pcol.set_hexpand(True)
        user_title = os.environ.get("USER", "User").capitalize()
        lbl_user = Gtk.Label(label=f"{user_title} • {socket.gethostname()}", xalign=0)
        lbl_user.add_css_class("profile-name")
        lbl_sub = Gtk.Label(label="AuraDE System Settings", xalign=0)
        lbl_sub.add_css_class("profile-sub")
        pcol.append(lbl_user)
        pcol.append(lbl_sub)
        prof.append(pcol)
        box.append(prof)

        sec_lbl = Gtk.Label(label="SYSTEM PREFERENCES", xalign=0)
        sec_lbl.add_css_class("sidebar-section-label")
        box.append(sec_lbl)

        lb = Gtk.ListBox()
        lb.set_selection_mode(Gtk.SelectionMode.SINGLE)
        lb.set_valign(Gtk.Align.START)
        lb.set_vexpand(False)

        for _name, label, icon_name, badge_class in PAGES:
            row = Gtk.ListBoxRow()
            rbox = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
            rbox.set_margin_top(4)
            rbox.set_margin_bottom(4)
            rbox.set_margin_start(6)
            rbox.set_margin_end(6)

            badge = make_badge(icon_name, badge_class, "sidebar-badge")
            rbox.append(badge)

            text = Gtk.Label(label=label, xalign=0)
            text.set_hexpand(True)
            text.set_valign(Gtk.Align.CENTER)
            rbox.append(text)

            row.set_child(rbox)
            lb.append(row)

        lb.connect("row-selected", self.on_page_selected)
        self.sidebar_list = lb

        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
        scroll.set_vexpand(True)
        scroll.set_child(lb)
        box.append(scroll)

        # Bottom compact status footer pinned at bottom of sidebar
        footer = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        footer.add_css_class("sidebar-footer")
        footer.set_vexpand(False)
        f_ic = Gtk.Image.new_from_icon_name("emblem-system-symbolic")
        f_ic.set_pixel_size(15)
        f_lbl = Gtk.Label(label="AuraDE Wayland Compositor", xalign=0)
        f_lbl.add_css_class("profile-sub")
        f_lbl.set_hexpand(True)
        footer.append(f_ic)
        footer.append(f_lbl)
        box.append(footer)

        return box

    def on_page_selected(self, _lb, row):
        if row is None:
            return
        idx = row.get_index()
        if 0 <= idx < len(PAGES):
            self.stack.set_visible_child_name(PAGES[idx][0])
            if hasattr(self, "header_title_lbl") and self.header_title_lbl:
                self.header_title_lbl.set_label(PAGES[idx][1])

    def page(self, title, lead):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        box.set_margin_start(28)
        box.set_margin_end(28)
        box.set_margin_top(22)
        box.set_margin_bottom(28)

        t = Gtk.Label(label=title, xalign=0)
        t.add_css_class("headline")
        box.append(t)

        if lead:
            l = Gtk.Label(label=lead, xalign=0)
            l.add_css_class("lead")
            l.set_wrap(True)
            box.append(l)

        return box

    def card(self, parent, title=None):
        if title:
            lbl = Gtk.Label(label=title.upper(), xalign=0)
            lbl.add_css_class("section")
            parent.append(lbl)
        c = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        c.add_css_class("card")
        parent.append(c)
        return c

    def add_row(self, card, title, subtitle, control, icon_name=None, badge_class="badge-wifi"):
        if card.get_first_child() is not None:
            sep = Gtk.Box()
            sep.add_css_class("mac-sep")
            card.append(sep)

        row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        row.add_css_class("row")

        if icon_name:
            badge = make_badge(icon_name, badge_class, "row-badge")
            row.append(badge)

        label_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        label_box.set_valign(Gtk.Align.CENTER)
        label_box.set_hexpand(True)

        t = Gtk.Label(label=title, xalign=0)
        t.add_css_class("title")
        label_box.append(t)

        if subtitle:
            s = Gtk.Label(label=subtitle, xalign=0)
            s.add_css_class("subtitle")
            s.set_wrap(True)
            label_box.append(s)

        row.append(label_box)
        if control is not None:
            control.set_valign(Gtk.Align.CENTER)
            row.append(control)
        card.append(row)
        return row

    def switch(self, section, key, on_change=None):
        sw = Gtk.Switch()
        sw.set_active(bool(self.cfg[section].get(key, False)))
        sw.connect("notify::active", self.on_switch, section, key, on_change)
        return sw

    def on_switch(self, sw, _param, section, key, on_change):
        self.cfg[section][key] = sw.get_active()
        if on_change:
            on_change(sw.get_active())
        self.changed()

    def scale(self, section, key, lo, hi, step, digits, on_change=None):
        sc = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, lo, hi, step)
        sc.set_digits(digits)
        sc.set_draw_value(True)
        sc.set_value_pos(Gtk.PositionType.RIGHT)
        sc.set_size_request(140, -1)
        sc.set_value(float(self.cfg[section].get(key, lo)))
        sc.connect("value-changed", self.on_scale, section, key, on_change)
        return sc

    def on_scale(self, sc, section, key, on_change):
        self.cfg[section][key] = round(sc.get_value(), 4)
        if on_change:
            on_change(sc.get_value())
        self.changed()

    def dropdown(self, section, key):
        dd = Gtk.DropDown.new_from_strings(ACTION_LABELS)
        value = self.cfg[section].get(key, "none")
        dd.set_selected(ACTION_INDEX.get(value, ACTION_INDEX["none"]))
        dd.connect("notify::selected", self.on_dropdown, section, key)
        return dd

    def on_dropdown(self, dd, _param, section, key):
        idx = dd.get_selected()
        if 0 <= idx < len(ACTION_VALUES):
            self.cfg[section][key] = ACTION_VALUES[idx]
        self.changed()

    def entry(self, section, key, placeholder=""):
        en = Gtk.Entry()
        en.set_text(self.cfg[section].get(key, ""))
        en.set_placeholder_text(placeholder)
        en.set_size_request(160, -1)
        en.connect("changed", self.on_entry, section, key)
        return en

    def on_entry(self, en, section, key):
        self.cfg[section][key] = en.get_text()
        self.changed()

    def changed(self):
        if self.loading:
            return
        if self.status:
            self.status.set_text("Saving…")
            self.status.set_css_classes(["disconnected-pill"])
        if self.save_source:
            GLib.source_remove(self.save_source)
        self.save_source = GLib.timeout_add(300, self.save_debounced)

    def save_debounced(self):
        self.save_source = 0
        self.save_now()
        return False

    def save_now(self):
        try:
            os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
            tmp_path = CONFIG_PATH + ".tmp"
            with open(tmp_path, "w") as f:
                json.dump(self.cfg, f, indent=2)
            os.replace(tmp_path, CONFIG_PATH)
            if self.status:
                self.status.set_text("Saved")
                self.status.set_css_classes(["connected-pill"])
        except Exception as exc:
            print("AuraDE settings: save failed:", exc)
            if self.status:
                self.status.set_text("Save failed!")

    # =======================================================================
    # PAGE: WI-FI
    # =======================================================================

    def page_wifi(self):
        box = self.page("Wi-Fi", "Connect to wireless networks, manage internet settings and passwords.")

        self._updating_wifi_ui = False
        self.wifi_connecting_ssid = None
        self.wifi_prompt_ssid = None
        self.wifi_saved_ssids = set()
        self.wifi_last_enabled = True
        self.wifi_last_active = None
        self.wifi_last_networks = []

        # Power & Scan Header Card
        card_power = self.card(box, "Wireless Power")
        self.wifi_switch = Gtk.Switch()
        self.wifi_switch.connect("notify::active", self.on_wifi_power_toggled)

        self.wifi_scan_btn = Gtk.Button()
        self.wifi_scan_btn.add_css_class("btn-primary")
        scan_btn_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        scan_btn_box.set_halign(Gtk.Align.CENTER)
        self.wifi_scan_spinner = Gtk.Spinner()
        self.wifi_scan_spinner.set_visible(False)
        self.wifi_scan_lbl = Gtk.Label(label="Scan for Networks")
        scan_btn_box.append(self.wifi_scan_spinner)
        scan_btn_box.append(self.wifi_scan_lbl)
        self.wifi_scan_btn.set_child(scan_btn_box)
        self.wifi_scan_btn.connect("clicked", lambda *_: self.refresh_wifi(rescan=True))

        hrow = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        hrow.append(self.wifi_scan_btn)
        hrow.append(self.wifi_switch)
        self.add_row(card_power, "Wi-Fi Interface", "Enable or disable wireless networking.", hrow,
                     icon_name="network-wireless-symbolic", badge_class="badge-wifi")

        # Animated inline Password Prompt Revealer
        self.wifi_pw_revealer = Gtk.Revealer()
        self.wifi_pw_revealer.set_transition_type(Gtk.RevealerTransitionType.SLIDE_DOWN)
        self.wifi_pw_revealer.set_transition_duration(220)
        self.wifi_pw_revealer.set_reveal_child(False)

        pw_outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        pw_card = self.card(pw_outer, "Authentication Required")
        pw_row = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        pw_row.set_margin_start(16)
        pw_row.set_margin_end(16)
        pw_row.set_margin_top(14)
        pw_row.set_margin_bottom(14)

        self.wifi_pw_title = Gtk.Label(label="Enter Wi-Fi Password", xalign=0)
        self.wifi_pw_title.add_css_class("title")
        pw_row.append(self.wifi_pw_title)

        pw_controls = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
        self.wifi_pw_entry = Gtk.PasswordEntry()
        self.wifi_pw_entry.set_show_peek_icon(True)
        self.wifi_pw_entry.set_hexpand(True)
        self.wifi_pw_entry.connect("activate", self.on_wifi_pw_submit)
        pw_controls.append(self.wifi_pw_entry)

        pw_cancel = Gtk.Button(label="Cancel")
        pw_cancel.connect("clicked", self.on_wifi_pw_cancel)
        pw_controls.append(pw_cancel)

        self.wifi_pw_join_btn = Gtk.Button(label="Join Network")
        self.wifi_pw_join_btn.add_css_class("btn-primary")
        self.wifi_pw_join_btn.connect("clicked", self.on_wifi_pw_submit)
        pw_controls.append(self.wifi_pw_join_btn)

        pw_row.append(pw_controls)
        pw_card.append(pw_row)
        self.wifi_pw_revealer.set_child(pw_outer)
        box.append(self.wifi_pw_revealer)

        # Active Network Card
        self.wifi_active_card = self.card(box, "Connected Network")
        self.wifi_active_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        self.wifi_active_card.append(self.wifi_active_box)

        # Available Networks Card
        self.wifi_list_card = self.card(box, "Available Networks")
        self.wifi_list_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        self.wifi_list_card.append(self.wifi_list_box)

        # Instant cached load (<16ms via --rescan no), followed by background hardware rescan
        self.refresh_wifi(rescan=False)
        return box

    def on_wifi_power_toggled(self, sw, _param):
        if getattr(self, "_updating_wifi_ui", False):
            return
        active = sw.get_active()
        if self.status:
            self.status.set_text("Turning Wi-Fi on…" if active else "Turning Wi-Fi off…")
        threading.Thread(
            target=lambda: subprocess.run(["nmcli", "radio", "wifi", "on" if active else "off"], capture_output=True),
            daemon=True
        ).start()
        GLib.timeout_add(600, lambda: self.refresh_wifi(rescan=False) or False)

    def _set_wifi_scanning_ui(self, scanning):
        if not hasattr(self, "wifi_scan_spinner"):
            return
        self.wifi_scan_spinner.set_visible(scanning)
        if scanning:
            self.wifi_scan_spinner.start()
            self.wifi_scan_lbl.set_label("Scanning…")
            self.wifi_scan_btn.set_sensitive(False)
        else:
            self.wifi_scan_spinner.stop()
            self.wifi_scan_lbl.set_label("Scan for Networks")
            self.wifi_scan_btn.set_sensitive(True)

    def refresh_wifi(self, rescan=False):
        if rescan:
            GLib.idle_add(self._set_wifi_scanning_ui, True)

        def worker():
            if rescan:
                subprocess.run(["nmcli", "dev", "wifi", "rescan"], capture_output=True)

            # Check power (5ms)
            res = subprocess.run(["nmcli", "radio", "wifi"], capture_output=True, text=True)
            enabled = "enabled" in res.stdout.strip().lower()

            saved_ssids = set()
            active_info = None
            networks = []

            if enabled:
                # Saved connection profiles (~8ms)
                cres = subprocess.run(["nmcli", "-t", "-f", "NAME", "connection", "show"], capture_output=True, text=True)
                for line in cres.stdout.splitlines():
                    name = line.strip()
                    if name:
                        saved_ssids.add(name)

                # Active connection details (~10ms)
                sres = subprocess.run(["nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "dev", "status"], capture_output=True, text=True)
                for line in sres.stdout.splitlines():
                    p = line.split(":")
                    if len(p) >= 4 and p[1] == "wifi" and "connected" in p[2]:
                        dev = p[0]
                        ssid = p[3]
                        ip, gateway = "—", "—"
                        ires = subprocess.run(["nmcli", "-t", "-f", "IP4.ADDRESS,IP4.GATEWAY", "dev", "show", dev], capture_output=True, text=True)
                        for iline in ires.stdout.splitlines():
                            if "IP4.ADDRESS" in iline and ip == "—":
                                ip = iline.split(":", 1)[1]
                            elif "IP4.GATEWAY" in iline:
                                gateway = iline.split(":", 1)[1]
                        active_info = {"device": dev, "ssid": ssid, "ip": ip, "gateway": gateway}
                        break

                # Use --rescan no for instant (<16ms) cached network list
                list_cmd = ["nmcli", "-t", "-f", "IN-USE,BSSID,SSID,SIGNAL,SECURITY", "dev", "wifi", "list"]
                if not rescan:
                    list_cmd.extend(["--rescan", "no"])
                nres = subprocess.run(list_cmd, capture_output=True, text=True)
                seen = set()
                for line in nres.stdout.splitlines():
                    clean = line.replace(r"\:", "__COLON__")
                    parts = clean.split(":")
                    if len(parts) >= 5:
                        in_use = parts[0].strip() == "*"
                        bssid = parts[1].replace("__COLON__", ":")
                        ssid = parts[2].replace("__COLON__", ":").strip()
                        if not ssid or ssid == "--":
                            continue
                        try:
                            sig = int(parts[3].strip())
                        except ValueError:
                            sig = 0
                        sec = parts[4].strip()
                        if ssid not in seen:
                            seen.add(ssid)
                            networks.append({
                                "ssid": ssid,
                                "bssid": bssid,
                                "signal": sig,
                                "security": sec,
                                "in_use": in_use,
                                "saved": ssid in saved_ssids,
                            })
                        elif in_use:
                            for item in networks:
                                if item["ssid"] == ssid:
                                    item["in_use"] = True
                                    item["signal"] = max(item["signal"], sig)

                networks.sort(key=lambda x: (not x["in_use"], -x["signal"]))

            GLib.idle_add(self._on_wifi_refreshed, enabled, active_info, networks, saved_ssids, rescan)

        threading.Thread(target=worker, daemon=True).start()

    def _on_wifi_refreshed(self, enabled, active_info, networks, saved_ssids, rescan):
        if rescan:
            self._set_wifi_scanning_ui(False)
        self.wifi_saved_ssids = saved_ssids
        self.wifi_last_enabled = enabled
        self.wifi_last_active = active_info
        self.wifi_last_networks = networks
        self.update_wifi_ui(enabled, active_info, networks)

    def update_wifi_ui(self, enabled, active_info, networks):
        if not self.win:
            return
        self._updating_wifi_ui = True
        self.wifi_switch.set_active(enabled)
        self._updating_wifi_ui = False

        # Clear active box
        while child := self.wifi_active_box.get_first_child():
            self.wifi_active_box.remove(child)

        if active_info:
            disc_btn = Gtk.Button(label="Disconnect")
            disc_btn.add_css_class("btn-destructive")
            disc_btn.connect("clicked", lambda *_: self.on_wifi_disconnect(active_info["device"]))
            self.add_row(
                self.wifi_active_box,
                active_info["ssid"],
                f"Connected  ·  IP: {active_info['ip']}  ·  Router: {active_info['gateway']}",
                disc_btn,
                icon_name="network-wireless-signal-excellent-symbolic",
                badge_class="badge-gestures",
            )
        else:
            self.add_row(
                self.wifi_active_box,
                "Not Connected" if enabled else "Wi-Fi is Turned Off",
                "Select a wireless network below to join." if enabled else "Turn on Wi-Fi above to browse networks.",
                None,
                icon_name="network-wireless-offline-symbolic",
                badge_class="badge-about",
            )

        # Clear networks box
        while child := self.wifi_list_box.get_first_child():
            self.wifi_list_box.remove(child)

        if not enabled:
            self.add_row(
                self.wifi_list_box,
                "Wireless Disabled",
                "Turn on Wi-Fi above to search for nearby networks.",
                None,
                icon_name="network-wireless-disabled-symbolic",
                badge_class="badge-about",
            )
            return

        if not networks:
            self.add_row(
                self.wifi_list_box,
                "No Networks Found",
                "Click 'Scan for Networks' above to search again.",
                None,
                icon_name="network-wireless-acquiring-symbolic",
                badge_class="badge-about",
            )
            return

        for net in networks:
            sig = net["signal"]
            icon_name = "network-wireless-signal-excellent-symbolic" if sig >= 75 else \
                        "network-wireless-signal-good-symbolic" if sig >= 50 else \
                        "network-wireless-signal-ok-symbolic" if sig >= 25 else \
                        "network-wireless-signal-weak-symbolic"
            sec_txt = net["security"] if net["security"] and net["security"] != "--" else "Open Network"
            saved_tag = "  ·  Saved" if net.get("saved") and not net["in_use"] else ""

            is_connecting = (self.wifi_connecting_ssid == net["ssid"])
            if is_connecting:
                ctrl = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
                sp = Gtk.Spinner()
                sp.start()
                lbl = Gtk.Label(label="Connecting…")
                lbl.add_css_class("subtitle")
                ctrl.append(sp)
                ctrl.append(lbl)
            elif net["in_use"]:
                ctrl = Gtk.Label(label="Connected")
                ctrl.add_css_class("connected-pill")
            else:
                ctrl = Gtk.Button(label="Join")
                ctrl.add_css_class("btn-primary")
                ctrl.connect("clicked", lambda *_, n=net: self.on_wifi_connect_clicked(n))

            self.add_row(
                self.wifi_list_box,
                net["ssid"],
                f"Signal: {sig}%  ·  {sec_txt}{saved_tag}",
                ctrl,
                icon_name=icon_name,
                badge_class="badge-gestures" if (net["in_use"] or is_connecting) else "badge-wifi",
            )

    def on_wifi_disconnect(self, dev):
        if self.status:
            self.status.set_text("Disconnecting Wi-Fi…")
            self.status.set_css_classes(["disconnected-pill"])

        def worker():
            subprocess.run(["nmcli", "dev", "disconnect", dev], capture_output=True)
            self.refresh_wifi(rescan=False)
            GLib.idle_add(lambda: self.status.set_text("Disconnected") if self.status else None)

        threading.Thread(target=worker, daemon=True).start()

    def on_wifi_pw_cancel(self, *_args):
        self.wifi_prompt_ssid = None
        self.wifi_pw_entry.set_text("")
        self.wifi_pw_revealer.set_reveal_child(False)

    def on_wifi_pw_submit(self, *_args):
        ssid = self.wifi_prompt_ssid
        pwd = self.wifi_pw_entry.get_text()
        if not ssid:
            return
        self.wifi_pw_revealer.set_reveal_child(False)
        self.wifi_pw_entry.set_text("")
        self.connect_to_wifi(ssid, password=pwd)

    def on_wifi_connect_clicked(self, net):
        ssid = net["ssid"]
        sec = net["security"]
        is_open = (not sec or sec == "--")
        if is_open or ssid in self.wifi_saved_ssids:
            self.wifi_pw_revealer.set_reveal_child(False)
            self.connect_to_wifi(ssid, password=None)
            return

        # Smoothly reveal inline password bar
        self.wifi_prompt_ssid = ssid
        self.wifi_pw_title.set_label(f"Enter password for “{ssid}”")
        self.wifi_pw_entry.set_text("")
        self.wifi_pw_revealer.set_reveal_child(True)
        self.wifi_pw_entry.grab_focus()

    def connect_to_wifi(self, ssid, password=None):
        self.wifi_connecting_ssid = ssid
        if self.status:
            self.status.set_text(f"Connecting to {ssid}…")
            self.status.set_css_classes(["disconnected-pill"])
        self.update_wifi_ui(self.wifi_last_enabled, self.wifi_last_active, self.wifi_last_networks)

        def worker():
            cmd = ["nmcli", "dev", "wifi", "connect", ssid]
            if password:
                cmd.extend(["password", password])
            res = subprocess.run(cmd, capture_output=True, text=True)
            ok = (res.returncode == 0)
            err_out = (res.stdout + " " + res.stderr).lower()
            need_secret = (not ok and not password and ("secret" in err_out or "password" in err_out))

            def finish():
                self.wifi_connecting_ssid = None
                if ok:
                    if self.status:
                        self.status.set_text(f"Connected to {ssid}")
                        self.status.set_css_classes(["connected-pill"])
                elif need_secret:
                    self.wifi_prompt_ssid = ssid
                    self.wifi_pw_title.set_label(f"Password required for “{ssid}”")
                    self.wifi_pw_entry.set_text("")
                    self.wifi_pw_revealer.set_reveal_child(True)
                    self.wifi_pw_entry.grab_focus()
                    if self.status:
                        self.status.set_text("Password required")
                        self.status.set_css_classes(["disconnected-pill"])
                else:
                    if self.status:
                        self.status.set_text("Connection failed — check password")
                        self.status.set_css_classes(["disconnected-pill"])
                self.refresh_wifi(rescan=False)

            GLib.idle_add(finish)

        threading.Thread(target=worker, daemon=True).start()

    # =======================================================================
    # PAGE: BLUETOOTH
    # =======================================================================

    def page_bluetooth(self):
        box = self.page("Bluetooth", "Manage wireless accessories, headphones, mice, and discovery.")

        self._updating_bt_ui = False
        self.bt_is_scanning = False
        self.bt_scan_proc = None
        self.bt_busy_mac = None
        self.bt_busy_action = None
        self.bt_last_powered = False
        self.bt_last_discoverable = False
        self.bt_last_devices = []

        # Power & Controls Card
        card_power = self.card(box, "Bluetooth Adapter")
        self.bt_switch = Gtk.Switch()
        self.bt_switch.connect("notify::active", self.on_bt_power_toggled)

        self.bt_scan_btn = Gtk.Button()
        self.bt_scan_btn.add_css_class("btn-primary")
        bt_scan_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        bt_scan_box.set_halign(Gtk.Align.CENTER)
        self.bt_scan_spinner = Gtk.Spinner()
        self.bt_scan_spinner.set_visible(False)
        self.bt_scan_lbl = Gtk.Label(label="Scan for Devices")
        bt_scan_box.append(self.bt_scan_spinner)
        bt_scan_box.append(self.bt_scan_lbl)
        self.bt_scan_btn.set_child(bt_scan_box)
        self.bt_scan_btn.connect("clicked", lambda *_: self.on_bt_scan_clicked())

        hrow = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        hrow.append(self.bt_scan_btn)
        hrow.append(self.bt_switch)
        self.add_row(card_power, "Bluetooth Power", "Enable or disable the Bluetooth controller.", hrow,
                     icon_name="bluetooth-symbolic", badge_class="badge-bluetooth")

        self.bt_disc_switch = Gtk.Switch()
        self.bt_disc_switch.connect("notify::active", self.on_bt_disc_toggled)
        self.add_row(card_power, "Discoverable", "Allow other devices to discover this computer.", self.bt_disc_switch,
                     icon_name="network-transmit-receive-symbolic", badge_class="badge-display")

        # Paired & Connected Devices Card
        self.bt_devices_card = self.card(box, "My Devices")
        self.bt_devices_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.bt_devices_card.append(self.bt_devices_box)

        self.refresh_bluetooth()
        return box

    def on_bt_power_toggled(self, sw, _param):
        if getattr(self, "_updating_bt_ui", False):
            return
        active = sw.get_active()
        if self.status:
            self.status.set_text("Turning Bluetooth on…" if active else "Turning Bluetooth off…")
        threading.Thread(
            target=lambda: subprocess.run(["bluetoothctl", "power", "on" if active else "off"], capture_output=True),
            daemon=True
        ).start()
        GLib.timeout_add(600, lambda: self.refresh_bluetooth() or False)

    def on_bt_disc_toggled(self, sw, _param):
        if getattr(self, "_updating_bt_ui", False):
            return
        active = sw.get_active()
        threading.Thread(
            target=lambda: subprocess.run(["bluetoothctl", "discoverable", "on" if active else "off"], capture_output=True),
            daemon=True
        ).start()

    def _stop_bt_scan(self):
        self.bt_is_scanning = False
        if self.bt_scan_proc is not None:
            try:
                self.bt_scan_proc.terminate()
            except Exception:
                pass
            self.bt_scan_proc = None
        if hasattr(self, "bt_scan_spinner"):
            self.bt_scan_spinner.stop()
            self.bt_scan_spinner.set_visible(False)
            self.bt_scan_lbl.set_label("Scan for Devices")

    def on_bt_scan_clicked(self):
        if self.bt_is_scanning:
            self._stop_bt_scan()
            self.refresh_bluetooth()
            return

        self.bt_is_scanning = True
        self.bt_scan_spinner.set_visible(True)
        self.bt_scan_spinner.start()
        self.bt_scan_lbl.set_label("Stop Scanning")
        if self.status:
            self.status.set_text("Scanning for Bluetooth devices…")

        try:
            self.bt_scan_proc = subprocess.Popen(
                ["bluetoothctl", "--timeout", "12", "scan", "on"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            self._stop_bt_scan()
            return

        def poll_scan():
            if not self.bt_is_scanning:
                return False
            if self.bt_scan_proc and self.bt_scan_proc.poll() is not None:
                self._stop_bt_scan()
                self.refresh_bluetooth()
                if self.status:
                    self.status.set_text("Scan complete")
                return False
            self.refresh_bluetooth()
            return True

        GLib.timeout_add(1500, poll_scan)

    def _guess_bt_icon(self, name):
        low = name.lower()
        if any(k in low for k in ("airpods", "buds", "headphone", "headset", "ear", "wh-", "wf-", "bose", "jbl", "audio", "speaker", "sound")):
            return "audio-headset-symbolic"
        if any(k in low for k in ("mouse", "mx master", "mx anywhere", "trackpad")):
            return "input-mouse-symbolic"
        if any(k in low for k in ("keyboard", "keychron", "k380", "magic keyboard")):
            return "input-keyboard-symbolic"
        if any(k in low for k in ("iphone", "galaxy", "pixel", "phone", "android")):
            return "phone-symbolic"
        return "bluetooth-symbolic"

    def refresh_bluetooth(self):
        def worker():
            # Fast adapter status check (~5ms)
            sres = subprocess.run(["bluetoothctl", "show"], capture_output=True, text=True)
            powered = False
            discoverable = False
            for line in sres.stdout.splitlines():
                line = line.strip()
                if line.startswith("Powered:"):
                    powered = "yes" in line.lower()
                elif line.startswith("Discoverable:"):
                    discoverable = "yes" in line.lower()

            devices = []
            if powered:
                # Fast single call for connected devices (~14ms) instead of N sequential info calls
                conn_res = subprocess.run(["bluetoothctl", "devices", "Connected"], capture_output=True, text=True)
                connected_macs = set()
                for line in conn_res.stdout.splitlines():
                    parts = line.strip().split(" ", 2)
                    if len(parts) >= 2 and parts[0] == "Device":
                        connected_macs.add(parts[1])

                paired_res = subprocess.run(["bluetoothctl", "devices", "Paired"], capture_output=True, text=True)
                paired_macs = set()
                for line in paired_res.stdout.splitlines():
                    parts = line.strip().split(" ", 2)
                    if len(parts) >= 2 and parts[0] == "Device":
                        paired_macs.add(parts[1])

                # Fast single call for all known/discovered devices (~4ms)
                dres = subprocess.run(["bluetoothctl", "devices"], capture_output=True, text=True)
                seen_macs = set()
                for line in (paired_res.stdout + "\n" + dres.stdout).splitlines():
                    parts = line.strip().split(" ", 2)
                    if len(parts) >= 3 and parts[0] == "Device":
                        mac = parts[1]
                        name = parts[2].strip()
                        if mac in seen_macs or not name:
                            continue
                        seen_macs.add(mac)
                        connected = mac in connected_macs
                        paired = mac in paired_macs
                        # Skip unnamed BLE beacons whose default BlueZ alias is just a MAC address
                        if not connected and not paired and re.fullmatch(r"([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}", name):
                            continue
                        icon = self._guess_bt_icon(name)
                        devices.append({"mac": mac, "name": name, "connected": connected, "paired": paired, "icon": icon})

                devices.sort(key=lambda d: (not d["connected"], not d.get("paired", False), d["name"].lower()))

            GLib.idle_add(self._on_bt_refreshed, powered, discoverable, devices)

        threading.Thread(target=worker, daemon=True).start()

    def _on_bt_refreshed(self, powered, discoverable, devices):
        self.bt_last_powered = powered
        self.bt_last_discoverable = discoverable
        self.bt_last_devices = devices
        self.update_bluetooth_ui(powered, discoverable, devices)

    def update_bluetooth_ui(self, powered, discoverable, devices):
        if not self.win:
            return
        self._updating_bt_ui = True
        self.bt_switch.set_active(powered)
        self.bt_disc_switch.set_active(discoverable)
        self._updating_bt_ui = False

        while child := self.bt_devices_box.get_first_child():
            self.bt_devices_box.remove(child)

        if not powered:
            self.add_row(
                self.bt_devices_box,
                "Bluetooth is Turned Off",
                "Turn on Bluetooth above to connect to wireless accessories.",
                None,
                icon_name="bluetooth-disabled-symbolic",
                badge_class="badge-about",
            )
            return

        if not devices:
            self.add_row(
                self.bt_devices_box,
                "Searching for Accessories…" if self.bt_is_scanning else "No Accessories Found",
                "Make sure your accessory is turned on and in pairing mode." if self.bt_is_scanning else "Click 'Scan for Devices' above to discover headphones, mice, or keyboards.",
                None,
                icon_name="bluetooth-symbolic",
                badge_class="badge-about",
            )
            return

        for dev in devices:
            mac = dev["mac"]
            is_busy = (self.bt_busy_mac == mac)
            act_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)

            if is_busy:
                sp = Gtk.Spinner()
                sp.start()
                action_lbl = "Disconnecting…" if self.bt_busy_action == "disconnect" else \
                             "Removing…" if self.bt_busy_action == "remove" else "Connecting…"
                lbl = Gtk.Label(label=action_lbl)
                lbl.add_css_class("subtitle")
                act_box.append(sp)
                act_box.append(lbl)
            elif dev["connected"]:
                pill = Gtk.Label(label="Connected")
                pill.add_css_class("connected-pill")
                act_box.append(pill)

                disc = Gtk.Button(label="Disconnect")
                disc.add_css_class("btn-destructive")
                disc.connect("clicked", lambda *_, d=dev: self.bt_device_action("disconnect", d["mac"], d["name"]))
                act_box.append(disc)
            else:
                conn = Gtk.Button(label="Connect")
                conn.add_css_class("btn-primary")
                conn.connect("clicked", lambda *_, d=dev: self.bt_device_action("connect", d["mac"], d["name"]))
                act_box.append(conn)

            if not is_busy:
                forget = Gtk.Button(label="Forget")
                forget.connect("clicked", lambda *_, d=dev: self.bt_device_action("remove", d["mac"], d["name"]))
                act_box.append(forget)

            status_sub = f"Connected  ·  {mac}" if dev["connected"] else f"Paired / Available  ·  {mac}"
            self.add_row(
                self.bt_devices_box,
                dev["name"],
                status_sub,
                act_box,
                icon_name=dev["icon"],
                badge_class="badge-gestures" if (dev["connected"] or is_busy) else "badge-bluetooth",
            )

    def bt_device_action(self, action, mac, name=None):
        disp = name or mac
        self.bt_busy_mac = mac
        self.bt_busy_action = action
        if self.status:
            verb = "Disconnecting" if action == "disconnect" else "Removing" if action == "remove" else "Connecting to"
            self.status.set_text(f"{verb} {disp}…")
            self.status.set_css_classes(["disconnected-pill"])
        self.update_bluetooth_ui(self.bt_last_powered, self.bt_last_discoverable, self.bt_last_devices)

        def worker():
            ok = False
            if action == "connect":
                subprocess.run(["bluetoothctl", "trust", mac], capture_output=True)
                res = subprocess.run(["bluetoothctl", "connect", mac], capture_output=True, text=True)
                if res.returncode != 0:
                    subprocess.run(["bluetoothctl", "pair", mac], capture_output=True, text=True)
                    res = subprocess.run(["bluetoothctl", "connect", mac], capture_output=True, text=True)
                ok = (res.returncode == 0 and "successful" in (res.stdout + res.stderr).lower()) or (res.returncode == 0)
            else:
                res = subprocess.run(["bluetoothctl", action, mac], capture_output=True, text=True)
                ok = (res.returncode == 0)

            def finish():
                self.bt_busy_mac = None
                self.bt_busy_action = None
                if self.status:
                    if ok:
                        msg = f"Connected to {disp}" if action == "connect" else \
                              f"Disconnected {disp}" if action == "disconnect" else f"Removed {disp}"
                        self.status.set_text(msg)
                        self.status.set_css_classes(["connected-pill" if action == "connect" else "disconnected-pill"])
                    else:
                        self.status.set_text(f"Could not {action} {disp}")
                        self.status.set_css_classes(["disconnected-pill"])
                self.refresh_bluetooth()

            GLib.idle_add(finish)

        threading.Thread(target=worker, daemon=True).start()

    # =======================================================================
    # PAGE: SOUND
    # =======================================================================

    def page_sound(self):
        box = self.page("Sound", "Control speaker volume, microphones, sound alerts and audio devices.")

        # Output Card
        card_out = self.card(box, "Output & Speakers")
        self.sound_out_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 100, 1)
        self.sound_out_scale.set_draw_value(True)
        self.sound_out_scale.set_size_request(220, -1)
        self.sound_out_scale.connect("value-changed", self.on_sound_out_volume)

        self.sound_out_mute = Gtk.Switch()
        self.sound_out_mute.connect("notify::active", self.on_sound_out_mute)

        test_btn = Gtk.Button(label="Play Sound Check")
        test_btn.add_css_class("btn-primary")
        test_btn.connect("clicked", self.on_play_test_sound)

        self.add_row(card_out, "Output Volume", "Adjust system audio playback volume.", self.sound_out_scale, icon_name="audio-volume-high-symbolic", badge_class="badge-sound")
        self.add_row(card_out, "Mute Output", "Silence all output audio streams.", self.sound_out_mute, icon_name="audio-volume-muted-symbolic", badge_class="badge-about")
        self.add_row(card_out, "Speaker Test", "Play a stereo reference chime on the active output.", test_btn, icon_name="audio-speakers-symbolic", badge_class="badge-dock")

        card_sinks = self.card(box, "Output Devices")
        self.sound_sinks_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        card_sinks.append(self.sound_sinks_box)

        # Input Card
        card_in = self.card(box, "Input & Microphone")
        self.sound_in_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, 100, 1)
        self.sound_in_scale.set_draw_value(True)
        self.sound_in_scale.set_size_request(220, -1)
        self.sound_in_scale.connect("value-changed", self.on_sound_in_volume)

        self.sound_in_mute = Gtk.Switch()
        self.sound_in_mute.connect("notify::active", self.on_sound_in_mute)

        self.add_row(card_in, "Input Volume", "Adjust recording and microphone gain.", self.sound_in_scale, icon_name="audio-input-microphone-symbolic", badge_class="badge-wifi")
        self.add_row(card_in, "Mute Microphone", "Disable microphone input across all apps.", self.sound_in_mute, icon_name="microphone-sensitivity-muted-symbolic", badge_class="badge-about")

        card_sources = self.card(box, "Input Devices")
        self.sound_sources_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        card_sources.append(self.sound_sources_box)

        self.refresh_sound()
        return box

    def on_sound_out_volume(self, sc):
        vol = sc.get_value() / 100.0
        subprocess.run(["wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", f"{vol:.2f}"], capture_output=True)

    def on_sound_out_mute(self, sw, _param):
        val = 1 if sw.get_active() else 0
        subprocess.run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", str(val)], capture_output=True)

    def on_sound_in_volume(self, sc):
        vol = sc.get_value() / 100.0
        subprocess.run(["wpctl", "set-volume", "@DEFAULT_AUDIO_SOURCE@", f"{vol:.2f}"], capture_output=True)

    def on_sound_in_mute(self, sw, _param):
        val = 1 if sw.get_active() else 0
        subprocess.run(["wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", str(val)], capture_output=True)

    def on_play_test_sound(self, _btn):
        def worker():
            snd = "/usr/share/sounds/freedesktop/stereo/audio-test-signal.oga"
            if not os.path.exists(snd):
                snd = "/usr/share/sounds/freedesktop/stereo/bell.oga"
            subprocess.run(["pw-play", snd], capture_output=True)
        threading.Thread(target=worker, daemon=True).start()

    def refresh_sound(self):
        def worker():
            out_vol = 50.0
            out_muted = False
            res = subprocess.run(["wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"], capture_output=True, text=True)
            if res.returncode == 0:
                txt = res.stdout.strip()
                out_muted = "[MUTED]" in txt
                p = txt.replace("[MUTED]", "").strip().split(":")
                if len(p) >= 2:
                    try: out_vol = float(p[1].strip()) * 100.0
                    except ValueError: pass

            in_vol = 80.0
            in_muted = False
            res = subprocess.run(["wpctl", "get-volume", "@DEFAULT_AUDIO_SOURCE@"], capture_output=True, text=True)
            if res.returncode == 0:
                txt = res.stdout.strip()
                in_muted = "[MUTED]" in txt
                p = txt.replace("[MUTED]", "").strip().split(":")
                if len(p) >= 2:
                    try: in_vol = float(p[1].strip()) * 100.0
                    except ValueError: pass

            sinks, sources = [], []
            res = subprocess.run(["wpctl", "status"], capture_output=True, text=True)
            section = None
            for line in res.stdout.splitlines():
                if "Sinks:" in line: section = "sinks"; continue
                elif "Sources:" in line: section = "sources"; continue
                elif any(x in line for x in ("Filters:", "Streams:", "Video", "Settings")): section = None; continue

                if section in ("sinks", "sources"):
                    lstr = line.replace("│", "").strip()
                    if not lstr or lstr.startswith("├") or lstr.startswith("└"): continue
                    is_def = "*" in lstr
                    clean = lstr.replace("*", "").strip()
                    if "." in clean:
                        id_str, rest = clean.split(".", 1)
                        try:
                            did = int(id_str.strip())
                            dname = rest.split("[")[0].strip()
                            item = {"id": did, "name": dname, "is_default": is_def}
                            if section == "sinks": sinks.append(item)
                            else: sources.append(item)
                        except ValueError: pass

            GLib.idle_add(self.update_sound_ui, out_vol, out_muted, sinks, in_vol, in_muted, sources)

        threading.Thread(target=worker, daemon=True).start()

    def update_sound_ui(self, out_vol, out_muted, sinks, in_vol, in_muted, sources):
        if not self.win:
            return
        self.sound_out_scale.set_value(out_vol)
        self.sound_out_mute.set_active(out_muted)
        self.sound_in_scale.set_value(in_vol)
        self.sound_in_mute.set_active(in_muted)

        # Populate output sinks
        while child := self.sound_sinks_box.get_first_child():
            self.sound_sinks_box.remove(child)

        for s in sinks:
            if s["is_default"]:
                ctrl = Gtk.Label(label="Default")
                ctrl.add_css_class("connected-pill")
            else:
                ctrl = Gtk.Button(label="Use Device")
                ctrl.connect("clicked", lambda *_, sid=s["id"]: self.set_default_sound_device(sid))
            self.add_row(
                self.sound_sinks_box,
                s["name"],
                "PipeWire Audio Output",
                ctrl,
                icon_name="audio-speakers-symbolic",
                badge_class="badge-sound" if s["is_default"] else "badge-about",
            )

        # Populate input sources
        while child := self.sound_sources_box.get_first_child():
            self.sound_sources_box.remove(child)

        for src in sources:
            if src["is_default"]:
                ctrl = Gtk.Label(label="Default")
                ctrl.add_css_class("connected-pill")
            else:
                ctrl = Gtk.Button(label="Use Device")
                ctrl.connect("clicked", lambda *_, sid=src["id"]: self.set_default_sound_device(sid))
            self.add_row(
                self.sound_sources_box,
                src["name"],
                "PipeWire Audio Input",
                ctrl,
                icon_name="audio-input-microphone-symbolic",
                badge_class="badge-wifi" if src["is_default"] else "badge-about",
            )

    def set_default_sound_device(self, dev_id):
        subprocess.run(["wpctl", "set-default", str(dev_id)], capture_output=True)
        GLib.timeout_add(300, lambda: self.refresh_sound() or False)

    # =======================================================================
    # PAGE: DISPLAYS
    # =======================================================================

    def page_display(self):
        box = self.page("Displays", "Screen resolution, brightness, Night Light and display scaling.")

        card_screen = self.card(box, "Screen Brightness & Resolution")

        # Brightness slider
        self.bright_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 5, 100, 1)
        self.bright_scale.set_draw_value(True)
        self.bright_scale.set_size_request(240, -1)
        self.bright_scale.connect("value-changed", self.on_brightness_changed)

        # Read current brightness
        b_res = subprocess.run(["brightnessctl", "-m"], capture_output=True, text=True)
        if b_res.returncode == 0:
            parts = b_res.stdout.strip().split(",")
            if len(parts) >= 4:
                try:
                    pct = int(parts[3].replace("%", ""))
                    self.bright_scale.set_value(pct)
                except ValueError:
                    self.bright_scale.set_value(50)
        self.add_row(card_screen, "Screen Brightness", "Adjust built-in panel backlighting.", self.bright_scale)

        # Display info & modes
        modes = []
        for mf in glob.glob("/sys/class/drm/*/modes"):
            try:
                with open(mf, "r") as f:
                    for line in f:
                        m = line.strip()
                        if m and m not in modes:
                            modes.append(m)
            except Exception:
                pass
        if not modes:
            modes = ["1920x1080", "1366x768", "1280x720"]

        mode_dd = Gtk.DropDown.new_from_strings(modes)
        mode_dd.set_selected(0)
        self.add_row(card_screen, "Display Resolution", "Active monitor video resolution mode.", mode_dd)

        # Night Light Card
        card_nl = self.card(box, "Night Light")
        nl_switch = Gtk.Switch()
        nl_active = bool(gsettings_get("org.gnome.settings-daemon.plugins.color", "night-light-enabled", False))
        nl_switch.set_active(nl_active)
        nl_switch.connect("notify::active", lambda sw, _: gsettings_set("org.gnome.settings-daemon.plugins.color", "night-light-enabled", sw.get_active()))
        self.add_row(card_nl, "Night Light", "Makes screen colours warmer to help prevent eye strain.", nl_switch)

        nl_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 1700, 5000, 100)
        nl_scale.set_draw_value(False)
        nl_scale.set_size_request(240, -1)
        cur_temp = gsettings_get("org.gnome.settings-daemon.plugins.color", "night-light-temperature", 3500)
        nl_scale.set_value(cur_temp if isinstance(cur_temp, (int, float)) else 3500)
        nl_scale.connect("value-changed", lambda sc: gsettings_set("org.gnome.settings-daemon.plugins.color", "night-light-temperature", int(sc.get_value())))
        self.add_row(card_nl, "Color Temperature", "Warmer colours reduce blue light emission.", nl_scale)

        # Display Scaling Card
        card_scale = self.card(box, "Display Scaling")
        scale_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        scale_box.set_margin_top(8); scale_box.set_margin_bottom(8); scale_box.set_margin_start(8)
        for s_val in ("100%", "125%", "150%", "200%"):
            sbtn = Gtk.Button(label=s_val)
            sbtn.connect("clicked", lambda *_, sv=s_val: self.on_scale_clicked(sv))
            scale_box.append(sbtn)
        self.add_row(card_scale, "UI Scale Factor", "Enlarge interface items for higher DPI screens.", scale_box)

        return box

    def on_brightness_changed(self, sc):
        val = int(sc.get_value())
        subprocess.run(["brightnessctl", "set", f"{val}%"], capture_output=True)

    def on_scale_clicked(self, s_val):
        if self.status:
            self.status.set_text(f"Display scale set to {s_val}")

    # =======================================================================
    # PAGE: WORKSPACES
    # =======================================================================

    def page_workspaces(self):
        box = self.page("Workspaces", "Configure virtual workspaces, add or delete spaces, and customize desktop switching.")

        card_count = self.card(box, "Workspace Count")

        ctrl_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)

        current_count = self.get_workspace_count()

        self.ws_spin = Gtk.SpinButton.new_with_range(1, 16, 1)
        self.ws_spin.set_value(current_count)
        self.ws_spin.set_digits(0)
        self.ws_spin.connect("value-changed", self.on_ws_spin_changed)

        btn_add = Gtk.Button(label="+ Add Space")
        btn_add.add_css_class("btn-primary")
        btn_add.connect("clicked", lambda _b: self.set_workspace_count(self.get_workspace_count() + 1))

        btn_del = Gtk.Button(label="− Delete Space")
        btn_del.add_css_class("btn-destructive")
        btn_del.connect("clicked", lambda _b: self.set_workspace_count(self.get_workspace_count() - 1))

        ctrl_box.append(self.ws_spin)
        ctrl_box.append(btn_add)
        ctrl_box.append(btn_del)

        self.add_row(card_count, "Number of Spaces", "Choose between 1 and 16 virtual desktops.", ctrl_box)

        card_visual = self.card(box, "Spaces Overview")

        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.NEVER)
        scroll.set_min_content_height(140)

        self.ws_cards_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        self.ws_cards_box.set_margin_top(8)
        self.ws_cards_box.set_margin_bottom(12)
        self.ws_cards_box.set_margin_start(4)
        self.ws_cards_box.set_margin_end(4)

        scroll.set_child(self.ws_cards_box)
        card_visual.append(scroll)

        self.refresh_workspace_cards()

        card_tiling = self.card(box, "Window Tiling (Hyprland Style)")
        self.add_row(
            card_tiling,
            "Dynamic Window Tiling",
            "Automatically arrange windows in a Hyprland-style Dwindle spiral layout (off by default). Press Super + V to float/tile a window, or Super + Drag to swap tiles.",
            self.switch("workspaces", "tiling"),
            icon_name="view-grid-symbolic",
            badge_class="badge-workspaces",
        )
        self.add_row(
            card_tiling,
            "Tiling Window Gaps",
            "Spacing in pixels between tiled windows and screen edges (0–32 px).",
            self.scale("workspaces", "tiling_gap", 0, 32, 1, 0),
            icon_name="preferences-system-windows-symbolic",
            badge_class="badge-appearance",
        )

        card_focus = self.card(box, "Window Focus & Blur")
        self.add_row(
            card_focus,
            "Auto-Focus on Space Switch",
            "Automatically focus the frontmost window when switching between workspaces.",
            self.switch("workspaces", "auto_focus"),
            icon_name="preferences-system-windows-symbolic",
            badge_class="badge-workspaces",
        )
        self.add_row(
            card_focus,
            "Overview Background Blur",
            "Enable frosted wallpaper blur in Mission Control.",
            self.switch("blur", "workspace_overview"),
            icon_name="preferences-desktop-wallpaper-symbolic",
            badge_class="badge-appearance",
        )

        card_nav = self.card(box, "Gestures & Navigation")
        self.add_row(card_nav, "Trackpad Switching", "Swipe left or right with 3 fingers to move smoothly between adjacent workspaces.", None)
        self.add_row(card_nav, "Mission Control", "Swipe up with 3 fingers to view all active workspaces in the top Spaces bar.", None)
        self.add_row(card_nav, "Keyboard Navigation", "Use Ctrl + Alt + Left/Right arrows to instantly switch workspaces.", None)

        return box

    def get_workspace_count(self):
        ws_cfg = self.cfg.setdefault("workspaces", {})
        return max(1, min(16, int(ws_cfg.get("count", 4))))

    def set_workspace_count(self, count):
        count = max(1, min(16, int(count)))
        old_count = self.get_workspace_count()
        if count == old_count:
            return
        self.cfg.setdefault("workspaces", {})["count"] = count
        if hasattr(self, "ws_spin") and self.ws_spin:
            self.ws_spin.set_value(count)
        self.refresh_workspace_cards()
        if self.status:
            if count > old_count:
                self.status.set_text(f"Added workspace (Total: {count})")
            else:
                self.status.set_text(f"Deleted workspace (Total: {count})")
        self.changed()

    def on_ws_spin_changed(self, spin):
        val = int(spin.get_value())
        if val != self.get_workspace_count():
            self.set_workspace_count(val)

    def refresh_workspace_cards(self):
        if not hasattr(self, "ws_cards_box") or not self.ws_cards_box:
            return

        while child := self.ws_cards_box.get_first_child():
            self.ws_cards_box.remove(child)

        count = self.get_workspace_count()

        for i in range(1, count + 1):
            c = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
            c.add_css_class("workspace-card")
            c.set_size_request(130, 110)

            top_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
            icon = Gtk.Image.new_from_icon_name("video-display-symbolic")
            top_row.append(icon)

            lbl_title = Gtk.Label(label=f"Space {i}", xalign=0)
            lbl_title.add_css_class("title")
            lbl_title.set_hexpand(True)
            top_row.append(lbl_title)

            c.append(top_row)

            if i == 1:
                sub = Gtk.Label(label="Primary Desktop", xalign=0)
                sub.add_css_class("subtitle")
                c.append(sub)
            else:
                sub = Gtk.Label(label="Virtual Desktop", xalign=0)
                sub.add_css_class("subtitle")
                c.append(sub)

            bottom_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
            bottom_box.set_vexpand(True)
            bottom_box.set_valign(Gtk.Align.END)

            if count > 1:
                btn_del = Gtk.Button(label="Delete")
                btn_del.add_css_class("btn-destructive")
                btn_del.set_tooltip_text(f"Delete Space {i}")
                btn_del.connect("clicked", lambda _b, idx=i: self.delete_workspace_at(idx))
                bottom_box.append(btn_del)
            else:
                lbl_min = Gtk.Label(label="Primary")
                lbl_min.add_css_class("subtitle")
                bottom_box.append(lbl_min)

            c.append(bottom_box)
            self.ws_cards_box.append(c)

        if count < 16:
            c_add = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
            c_add.add_css_class("workspace-card-add")
            c_add.set_size_request(120, 110)
            c_add.set_valign(Gtk.Align.CENTER)

            btn_plus = Gtk.Button(label="+ Add Space")
            btn_plus.add_css_class("btn-primary")
            btn_plus.set_valign(Gtk.Align.CENTER)
            btn_plus.set_vexpand(True)
            btn_plus.connect("clicked", lambda _b: self.set_workspace_count(self.get_workspace_count() + 1))
            c_add.append(btn_plus)

            self.ws_cards_box.append(c_add)

    def delete_workspace_at(self, _index):
        count = self.get_workspace_count()
        if count <= 1:
            return
        self.set_workspace_count(count - 1)

    # =======================================================================
    # PAGE: MOUSE & TOUCHPAD
    # =======================================================================

    def page_mouse(self):
        box = self.page("Mouse & Touchpad", "Pointer speed, natural scrolling, tap-to-click and gesture acceleration.")

        # Touchpad Card
        card_tp = self.card(box, "Touchpad")

        tp_tap = Gtk.Switch()
        tp_tap.set_active(bool(gsettings_get("org.gnome.desktop.peripherals.touchpad", "tap-to-click", True)))
        tp_tap.connect("notify::active", lambda sw, _: gsettings_set("org.gnome.desktop.peripherals.touchpad", "tap-to-click", sw.get_active()))
        self.add_row(card_tp, "Tap to Click", "Tap the touchpad with one finger to click.", tp_tap)

        tp_nat = Gtk.Switch()
        tp_nat.set_active(bool(gsettings_get("org.gnome.desktop.peripherals.touchpad", "natural-scroll", True)))
        tp_nat.connect("notify::active", self.on_touchpad_natural_scroll)
        self.add_row(card_tp, "Natural Scrolling", "Content tracks finger movement like macOS.", tp_nat)

        tp_speed = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, -1.0, 1.0, 0.1)
        tp_speed.set_draw_value(True)
        tp_speed.set_size_request(240, -1)
        val = gsettings_get("org.gnome.desktop.peripherals.touchpad", "speed", 0.0)
        tp_speed.set_value(float(val) if isinstance(val, (int, float)) else 0.0)
        tp_speed.connect("value-changed", lambda sc: gsettings_set("org.gnome.desktop.peripherals.touchpad", "speed", round(sc.get_value(), 2)))
        self.add_row(card_tp, "Tracking Speed", "Adjust touchpad pointer acceleration.", tp_speed)

        tp_dwt = Gtk.Switch()
        dwt_val = self.cfg.get("physics", {}).get("disable_while_typing", None)
        if dwt_val is None:
            dwt_val = bool(gsettings_get("org.gnome.desktop.peripherals.touchpad", "disable-while-typing", False))
        tp_dwt.set_active(bool(dwt_val))
        tp_dwt.connect("notify::active", self.on_touchpad_dwt)
        self.add_row(card_tp, "Lock Touchpad While Typing", "Reject palm touches while typing (turn off for gaming).", tp_dwt)

        # Mouse Card
        card_mouse = self.card(box, "Physical Mouse")

        m_speed = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, -1.0, 1.0, 0.1)
        m_speed.set_draw_value(True)
        m_speed.set_size_request(240, -1)
        mval = gsettings_get("org.gnome.desktop.peripherals.mouse", "speed", 0.0)
        m_speed.set_value(float(mval) if isinstance(mval, (int, float)) else 0.0)
        m_speed.connect("value-changed", lambda sc: gsettings_set("org.gnome.desktop.peripherals.mouse", "speed", round(sc.get_value(), 2)))
        self.add_row(card_mouse, "Mouse Speed", "Adjust optical pointer speed.", m_speed)

        m_left = Gtk.Switch()
        m_left.set_active(bool(gsettings_get("org.gnome.desktop.peripherals.mouse", "left-handed", False)))
        m_left.connect("notify::active", lambda sw, _: gsettings_set("org.gnome.desktop.peripherals.mouse", "left-handed", sw.get_active()))
        self.add_row(card_mouse, "Left-Handed Mouse", "Swap primary left and right mouse buttons.", m_left)

        m_accel = Gtk.DropDown.new_from_strings(["Default", "Flat (No Acceleration)", "Adaptive"])
        cur_accel = gsettings_get("org.gnome.desktop.peripherals.mouse", "accel-profile", "default")
        idx = 1 if cur_accel == "flat" else 2 if cur_accel == "adaptive" else 0
        m_accel.set_selected(idx)
        m_accel.connect("notify::selected", self.on_mouse_accel_changed)
        self.add_row(card_mouse, "Acceleration Profile", "Control pointer acceleration curves.", m_accel)

        return box

    def on_touchpad_natural_scroll(self, sw, _param):
        active = sw.get_active()
        gsettings_set("org.gnome.desktop.peripherals.touchpad", "natural-scroll", active)
        self.cfg["physics"]["natural_scroll"] = active
        self.changed()

    def on_touchpad_dwt(self, sw, _param):
        active = sw.get_active()
        gsettings_set("org.gnome.desktop.peripherals.touchpad", "disable-while-typing", active)
        self.cfg.setdefault("physics", {})["disable_while_typing"] = active
        self.cfg.setdefault("touchpad", {})["disable_while_typing"] = active
        self.save_now()

    def on_mouse_accel_changed(self, dd, _param):
        idx = dd.get_selected()
        mode = "flat" if idx == 1 else "adaptive" if idx == 2 else "default"
        gsettings_set("org.gnome.desktop.peripherals.mouse", "accel-profile", mode)

    # =======================================================================
    # PAGE: KEYBOARD
    # =======================================================================

    def page_keyboard(self):
        box = self.page("Keyboard", "Configure layout, input sources, key repeat delays and system shortcuts.")

        card_layout = self.card(box, "Input Sources & Layout")
        self.add_row(card_layout, "Layouts", "Comma separated XKB layouts, e.g. us,ua",
                     self.entry("keyboard", "layout", "us"))
        self.add_row(card_layout, "Variants", "One variant per layout, e.g. ,winkeysenhanced",
                     self.entry("keyboard", "variant", ""))
        self.add_row(card_layout, "Model", "Usually pc105",
                     self.entry("keyboard", "model", "pc105"))
        self.add_row(card_layout, "Options", "XKB options, e.g. grp:alt_shift_toggle",
                     self.entry("keyboard", "options", ""))

        card_repeat = self.card(box, "Key Repeat")
        del_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 150, 1000, 25)
        del_scale.set_draw_value(True)
        del_scale.set_size_request(240, -1)
        cur_del = gsettings_get("org.gnome.desktop.peripherals.keyboard", "delay", 500)
        del_scale.set_value(float(cur_del) if isinstance(cur_del, (int, float)) else 500)
        del_scale.connect("value-changed", lambda sc: gsettings_set("org.gnome.desktop.peripherals.keyboard", "delay", int(sc.get_value())))
        self.add_row(card_repeat, "Repeat Delay", "Delay until repeated key strokes begin (ms).", del_scale)

        rep_scale = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 10, 100, 5)
        rep_scale.set_draw_value(True)
        rep_scale.set_size_request(240, -1)
        cur_rep = gsettings_get("org.gnome.desktop.peripherals.keyboard", "repeat-interval", 30)
        rep_scale.set_value(float(cur_rep) if isinstance(cur_rep, (int, float)) else 30)
        rep_scale.connect("value-changed", lambda sc: gsettings_set("org.gnome.desktop.peripherals.keyboard", "repeat-interval", int(sc.get_value())))
        self.add_row(card_repeat, "Repeat Interval", "Speed at which keys repeat while held down (ms).", rep_scale)

        card_shortcuts = self.card(box, "Keyboard & Gesture Shortcuts")
        shortcuts = [
            ("Switch Layout", "Super + Space"),
            ("Toggle Floating (Tiling)", "Super + V"),
            ("Mission Control", "Swipe 3 fingers Up · F3"),
            ("App Exposé", "Swipe 3 fingers Down"),
            ("Switch Workspaces", "Swipe 3 fingers Left / Right"),
            ("Launchpad", "Pinch 4 fingers In · F4"),
            ("Show Desktop", "Spread 4 fingers Out"),
            ("Control Center", "Super + C · Click Topbar Pill"),
            ("Full Screenshot", "Print"),
            ("Area Screenshot", "Shift + Print"),
            ("Lock Screen", "Super + L"),
        ]
        for name, shortcut in shortcuts:
            slbl = Gtk.Label(label=shortcut)
            slbl.add_css_class("mono")
            self.add_row(card_shortcuts, name, None, slbl, icon_name="input-keyboard-symbolic", badge_class="badge-keyboard")

        return box

    # =======================================================================
    # PAGE: APPEARANCE
    # =======================================================================

    def page_appearance(self):
        box = self.page("Appearance", "Theme, accent colour, cursor styling and desktop wallpaper.")

        card_theme = self.card(box, "Theme & Colors")
        dark_switch = Gtk.Switch()
        live_dark = gsettings_get("org.gnome.desktop.interface", "color-scheme", "")
        is_dark = "dark" in str(live_dark).lower() if live_dark else self.cfg["appearance"].get("dark_mode", True)
        dark_switch.set_active(bool(is_dark))
        dark_switch.connect("notify::active", self.on_dark_mode_switch)
        self.add_row(card_theme, "Dark Mode", "Switch between dark glass and light glass aesthetic.", dark_switch, icon_name="weather-clear-night-symbolic", badge_class="badge-appearance")

        color = Gtk.ColorButton()
        color.set_rgba(hex_rgba(self.cfg["appearance"].get("accent_color", "#007AFF")))
        color.set_use_alpha(False)
        color.connect("color-set", self.on_accent)
        self.add_row(card_theme, "Accent Colour", "Used for toggles, highlights and Control Center controls.", color, icon_name="applications-graphics-symbolic", badge_class="badge-wifi")

        # Cursor Theme & Size Card
        card_cursor = self.card(box, "Cursor & Pointer")
        cursor_themes = ["Aura-Circle", "WhiteSur-cursors", "Adwaita", "Papirus"]
        cur_theme = gsettings_get("org.gnome.desktop.interface", "cursor-theme", "Aura-Circle")
        c_dd = Gtk.DropDown.new_from_strings(cursor_themes)
        c_idx = cursor_themes.index(cur_theme) if cur_theme in cursor_themes else 0
        c_dd.set_selected(c_idx)
        c_dd.connect("notify::selected", lambda dd, _: self.on_cursor_theme_changed(cursor_themes[dd.get_selected()]))
        self.add_row(card_cursor, "Cursor Theme", "Choose the system-wide pointer appearance.", c_dd, icon_name="input-mouse-symbolic", badge_class="badge-mouse")

        c_size_dd = Gtk.DropDown.new_from_strings(["24 px", "32 px", "48 px"])
        cur_size = gsettings_get("org.gnome.desktop.interface", "cursor-size", 24)
        s_idx = 0 if cur_size <= 24 else 1 if cur_size <= 32 else 2
        c_size_dd.set_selected(s_idx)
        c_size_dd.connect("notify::selected", lambda dd, _: self.on_cursor_size_changed([24, 32, 48][dd.get_selected()]))
        self.add_row(card_cursor, "Cursor Size", "Scale pointer icon size for readability.", c_size_dd, icon_name="zoom-fit-best-symbolic", badge_class="badge-workspaces")

        # Wallpaper Card
        card_wp = self.card(box, "Desktop Wallpaper")
        self.wallpaper_label = Gtk.Label(xalign=0)
        self.wallpaper_label.add_css_class("mono")
        self.wallpaper_label.set_text(self.cfg["appearance"].get("wallpaper_path") or "Built-in dynamic gradient")

        buttons = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        choose = Gtk.Button(label="Choose Image…")
        choose.add_css_class("btn-primary")
        choose.connect("clicked", self.on_choose_wallpaper)
        buttons.append(choose)
        clear = Gtk.Button(label="Use Built-in")
        clear.connect("clicked", self.on_clear_wallpaper)
        buttons.append(clear)

        self.add_row(card_wp, "Active Wallpaper", "Select a custom background image or use the built-in AuraDE gradient.", buttons, icon_name="preferences-desktop-wallpaper-symbolic", badge_class="badge-appearance")
        self.add_row(card_wp, "Current Source", None, self.wallpaper_label)

        # Dynamic Island / Music Pill
        card_music = self.card(box, "Music Pill (Dynamic Island)")
        if "music_pill" not in self.cfg:
            self.cfg["music_pill"] = {"enabled": True, "position": "topbar_center"}

        sw_mp = Gtk.Switch()
        sw_mp.set_active(bool(self.cfg["music_pill"].get("enabled", True)))
        sw_mp.connect("notify::active", lambda s, _: self.on_music_pill_switch(s.get_active()))
        self.add_row(card_music, "Show Music Pill", "Display an interactive dynamic island music pill when media is playing.", sw_mp, icon_name="audio-x-generic-symbolic", badge_class="badge-sound")

        pos_labels = ["Top Bar (Center)", "Dock (Left)", "Dock (Right)", "Disabled"]
        pos_values = ["topbar_center", "dock_left", "dock_right", "disabled"]
        cur_pos = self.cfg["music_pill"].get("position", "topbar_center")
        pos_idx = pos_values.index(cur_pos) if cur_pos in pos_values else 0
        pos_dd = Gtk.DropDown.new_from_strings(pos_labels)
        pos_dd.set_selected(pos_idx)
        pos_dd.connect("notify::selected", lambda dd, _: self.on_music_pill_pos(pos_values[dd.get_selected()]))
        self.add_row(card_music, "Pill Placement", "Choose where the Dynamic Island appears (Top Bar Center, Dock Left or Right).", pos_dd, icon_name="preferences-system-symbolic", badge_class="badge-appearance")

        # Frosted Glass & Blur Effects
        card_blur = self.card(box, "Frosted Glass & Blur Effects")
        self.add_row(card_blur, "Blur Strength", "Adjust the intensity and diffusion radius of the frosted glass blur.", self.scale("blur", "strength", 0.2, 2.5, 0.1, 1), icon_name="preferences-desktop-theme-symbolic", badge_class="badge-appearance")
        self.add_row(card_blur, "Window Blur", "Frosted glass background blur under translucent application windows.", self.switch("blur", "windows"), icon_name="preferences-system-windows-symbolic", badge_class="badge-workspaces")
        self.add_row(card_blur, "Dock Blur", "Frosted glass background blur under the application dock.", self.switch("blur", "dock"), icon_name="view-paged-symbolic", badge_class="badge-dock")
        self.add_row(card_blur, "Top Bar Blur", "Frosted glass background blur under the top menu bar.", self.switch("blur", "topbar"), icon_name="preferences-desktop-display-symbolic", badge_class="badge-appearance")
        self.add_row(card_blur, "Launchpad Blur", "Frosted glass wallpaper blur when Launchpad is open.", self.switch("blur", "launchpad"), icon_name="view-grid-symbolic", badge_class="badge-workspaces")
        self.add_row(card_blur, "Workspace Overview Blur", "Frosted glass wallpaper blur in Mission Control.", self.switch("blur", "workspace_overview"), icon_name="preferences-system-windows-symbolic", badge_class="badge-gestures")

        # Transparency & Opacity
        card_opacity = self.card(box, "Transparency & Opacity")
        self.add_row(card_opacity, "Dock Opacity", "Adjust background transparency of the dock (0.0 = clear glass, 1.0 = solid).", self.scale("dock", "opacity", 0.0, 1.0, 0.05, 2), icon_name="view-paged-symbolic", badge_class="badge-dock")
        self.add_row(card_opacity, "Global Window Opacity", "Base opacity applied to all windows unless customized below.", self.scale("appearance", "window_opacity", 0.2, 1.0, 0.05, 2), icon_name="preferences-system-windows-symbolic", badge_class="badge-workspaces")

        self.build_window_opacity_section(box)

        return box

    def build_window_opacity_section(self, box):
        header_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        header_box.set_hexpand(True)
        lbl = Gtk.Label(label="RUNNING WINDOWS OPACITY", xalign=0)
        lbl.add_css_class("section")
        lbl.set_hexpand(True)
        header_box.append(lbl)

        btn_refresh = Gtk.Button(label="Refresh")
        btn_refresh.set_icon_name("view-refresh-symbolic")
        btn_refresh.add_css_class("btn-secondary")
        btn_refresh.connect("clicked", lambda _: self.populate_running_windows())
        header_box.append(btn_refresh)
        box.append(header_box)

        self.win_opacity_card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.win_opacity_card.add_css_class("card")
        box.append(self.win_opacity_card)

        self.populate_running_windows()

    def populate_running_windows(self):
        while child := self.win_opacity_card.get_first_child():
            self.win_opacity_card.remove(child)

        appr = self.cfg.setdefault("appearance", {})
        win_opacities = appr.setdefault("window_opacities", {})
        default_opacity = float(appr.get("window_opacity", 1.0))

        windows = []
        if os.path.exists("/tmp/aurade-windows.json"):
            try:
                with open("/tmp/aurade-windows.json", "r") as f:
                    windows = json.load(f)
            except Exception:
                windows = []

        seen_apps = set()

        if not windows and not win_opacities:
            row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
            row.add_css_class("row")
            badge = make_badge("preferences-system-windows-symbolic", "badge-workspaces", "row-badge")
            row.append(badge)

            lbl_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
            lbl_box.set_valign(Gtk.Align.CENTER)
            lbl_box.set_hexpand(True)
            t = Gtk.Label(label="No Open Windows Detected", xalign=0)
            t.add_css_class("title")
            lbl_box.append(t)
            s = Gtk.Label(label="Open applications will appear here to adjust individual opacity.", xalign=0)
            s.add_css_class("subtitle")
            lbl_box.append(s)
            row.append(lbl_box)
            self.win_opacity_card.append(row)
            return

        display = Gdk.Display.get_default()
        theme = Gtk.IconTheme.get_for_display(display) if display else None

        for w in windows:
            app_id = w.get("app_id", "") or "unknown"
            title = w.get("title", "")
            workspace = w.get("workspace", 0)
            minimized = w.get("minimized", False)
            seen_apps.add(app_id)

            icon_name = "preferences-system-windows-symbolic"
            if theme and app_id:
                for candidate in [app_id, app_id.lower(), app_id.split(".")[-1].lower()]:
                    if theme.has_icon(candidate):
                        icon_name = candidate
                        break

            cur_op = win_opacities.get(app_id, default_opacity)

            if self.win_opacity_card.get_first_child() is not None:
                sep = Gtk.Box()
                sep.add_css_class("mac-sep")
                self.win_opacity_card.append(sep)

            row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
            row.add_css_class("row")

            badge = make_badge(icon_name, "badge-workspaces", "row-badge")
            row.append(badge)

            label_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
            label_box.set_valign(Gtk.Align.CENTER)
            label_box.set_hexpand(True)

            display_title = title if title else app_id.capitalize()
            t = Gtk.Label(label=display_title, xalign=0)
            t.add_css_class("title")
            t.set_ellipsize(Pango.EllipsizeMode.END)
            t.set_max_width_chars(32)
            label_box.append(t)

            status_str = f"Space {workspace + 1}"
            if minimized:
                status_str += " • Minimized"
            if app_id and app_id != display_title:
                status_str = f"{app_id} • {status_str}"
            s = Gtk.Label(label=status_str, xalign=0)
            s.add_css_class("subtitle")
            label_box.append(s)
            row.append(label_box)

            ctrl_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
            ctrl_box.set_valign(Gtk.Align.CENTER)

            sc = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.20, 1.0, 0.05)
            sc.set_digits(2)
            sc.set_draw_value(False)
            sc.set_size_request(160, -1)
            sc.set_value(float(cur_op))

            pct_lbl = Gtk.Label(label=f"{int(round(float(cur_op) * 100))}%")
            pct_lbl.add_css_class("mono")
            pct_lbl.set_size_request(45, -1)
            pct_lbl.set_xalign(1.0)

            btn_reset = Gtk.Button(label="Default")
            btn_reset.set_tooltip_text("Reset to global window opacity")
            btn_reset.add_css_class("btn-secondary")

            def on_slider_change(scale, aid=app_id, pl=pct_lbl):
                val = round(scale.get_value(), 2)
                pl.set_text(f"{int(round(val * 100))}%")
                self.cfg["appearance"].setdefault("window_opacities", {})[aid] = val
                rules = self.cfg["appearance"].setdefault("window_rules", [])
                found = False
                for r in rules:
                    if r.get("app_id") == aid and not r.get("title"):
                        r["opacity"] = val
                        found = True
                        break
                if not found:
                    rules.append({"app_id": aid, "title": "", "opacity": val})
                self.changed()

            def on_reset_clicked(_btn, aid=app_id, scale=sc, pl=pct_lbl):
                def_op = float(self.cfg["appearance"].get("window_opacity", 1.0))
                scale.set_value(def_op)
                pl.set_text(f"{int(round(def_op * 100))}%")
                if aid in self.cfg["appearance"].get("window_opacities", {}):
                    del self.cfg["appearance"]["window_opacities"][aid]
                rules = self.cfg["appearance"].get("window_rules", [])
                self.cfg["appearance"]["window_rules"] = [
                    r for r in rules if r.get("app_id") != aid or r.get("title")
                ]
                self.changed()

            sc.connect("value-changed", on_slider_change)
            btn_reset.connect("clicked", on_reset_clicked)

            ctrl_box.append(sc)
            ctrl_box.append(pct_lbl)
            ctrl_box.append(btn_reset)
            row.append(ctrl_box)

            self.win_opacity_card.append(row)

        for saved_app, saved_op in list(win_opacities.items()):
            if saved_app in seen_apps:
                continue

            if self.win_opacity_card.get_first_child() is not None:
                sep = Gtk.Box()
                sep.add_css_class("mac-sep")
                self.win_opacity_card.append(sep)

            row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
            row.add_css_class("row")

            badge = make_badge("preferences-system-windows-symbolic", "badge-dock", "row-badge")
            row.append(badge)

            label_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
            label_box.set_valign(Gtk.Align.CENTER)
            label_box.set_hexpand(True)

            t = Gtk.Label(label=saved_app.capitalize(), xalign=0)
            t.add_css_class("title")
            label_box.append(t)

            s = Gtk.Label(label=f"{saved_app} • (Not currently running)", xalign=0)
            s.add_css_class("subtitle")
            label_box.append(s)
            row.append(label_box)

            ctrl_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=10)
            ctrl_box.set_valign(Gtk.Align.CENTER)

            sc = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.20, 1.0, 0.05)
            sc.set_digits(2)
            sc.set_draw_value(False)
            sc.set_size_request(160, -1)
            sc.set_value(float(saved_op))

            pct_lbl = Gtk.Label(label=f"{int(round(float(saved_op) * 100))}%")
            pct_lbl.add_css_class("mono")
            pct_lbl.set_size_request(45, -1)
            pct_lbl.set_xalign(1.0)

            btn_reset = Gtk.Button(label="Remove")
            btn_reset.set_tooltip_text("Remove custom opacity override")
            btn_reset.add_css_class("btn-secondary")

            def on_saved_slider_change(scale, aid=saved_app, pl=pct_lbl):
                val = round(scale.get_value(), 2)
                pl.set_text(f"{int(round(val * 100))}%")
                self.cfg["appearance"].setdefault("window_opacities", {})[aid] = val
                rules = self.cfg["appearance"].setdefault("window_rules", [])
                found = False
                for r in rules:
                    if r.get("app_id") == aid and not r.get("title"):
                        r["opacity"] = val
                        found = True
                        break
                if not found:
                    rules.append({"app_id": aid, "title": "", "opacity": val})
                self.changed()

            def on_saved_remove_clicked(_btn, aid=saved_app):
                if aid in self.cfg["appearance"].get("window_opacities", {}):
                    del self.cfg["appearance"]["window_opacities"][aid]
                rules = self.cfg["appearance"].get("window_rules", [])
                self.cfg["appearance"]["window_rules"] = [
                    r for r in rules if r.get("app_id") != aid or r.get("title")
                ]
                self.changed()
                self.populate_running_windows()

            sc.connect("value-changed", on_saved_slider_change)
            btn_reset.connect("clicked", on_saved_remove_clicked)

            ctrl_box.append(sc)
            ctrl_box.append(pct_lbl)
            ctrl_box.append(btn_reset)
            row.append(ctrl_box)

            self.win_opacity_card.append(row)

    def on_dark_mode_switch(self, sw, _param):
        active = sw.get_active()
        gsettings_set("org.gnome.desktop.interface", "color-scheme", "prefer-dark" if active else "prefer-light")
        self.cfg["appearance"]["dark_mode"] = active
        self.apply_theme_class()
        self.changed()

    def on_accent(self, button):
        self.cfg["appearance"]["accent_color"] = rgba_hex(button.get_rgba())
        self.changed()

    def on_cursor_theme_changed(self, theme_name):
        gsettings_set("org.gnome.desktop.interface", "cursor-theme", theme_name)
        if self.status:
            self.status.set_text(f"Cursor theme set to {theme_name}")

    def on_cursor_size_changed(self, size):
        gsettings_set("org.gnome.desktop.interface", "cursor-size", size)
        if self.status:
            self.status.set_text(f"Cursor size set to {size}px")

    def on_choose_wallpaper(self, _button):
        dialog = Gtk.FileDialog()
        dialog.set_title("Choose a desktop wallpaper")
        try:
            dialog.open(self.win, None, self.on_wallpaper_chosen)
        except Exception as exc:
            print("File chooser error:", exc)

    def on_wallpaper_chosen(self, dialog, result, _data=None):
        try:
            gfile = dialog.open_finish(result)
        except GLib.Error:
            return
        if gfile is None:
            return
        path = gfile.get_path() or ""
        self.cfg["appearance"]["wallpaper_path"] = path
        self.wallpaper_label.set_text(path or "Built-in dynamic gradient")
        self.changed()

    def on_clear_wallpaper(self, _button):
        self.cfg["appearance"]["wallpaper_path"] = ""
        self.wallpaper_label.set_text("Built-in dynamic gradient")
        self.changed()

    # =======================================================================
    # PAGE: DOCK
    # =======================================================================

    def page_dock(self):
        box = self.page("Dock", "Autohide, intellihide, icon sizing, and magnification.")

        card_theme = self.card(box, "Theme & Style")
        dock_theme_labels = ["Modern Floating Glass Pill", "Classic 3D Glass Shelf (OS X Table)", "Plank Base Shelf"]
        dock_theme_values = ["modern", "table_3d", "plank"]
        cur_dock_theme = self.cfg.get("dock", {}).get("theme", "modern")
        dock_theme_idx = dock_theme_values.index(cur_dock_theme) if cur_dock_theme in dock_theme_values else 0
        dock_theme_dd = Gtk.DropDown.new_from_strings(dock_theme_labels)
        dock_theme_dd.set_selected(dock_theme_idx)
        dock_theme_dd.connect("notify::selected", lambda dd, _: self.on_dock_theme(dock_theme_values[dd.get_selected()]))
        self.add_row(card_theme, "Dock Theme", "Choose between modern floating pill, classic OS X 3D glass table shelf, or Plank shelf.", dock_theme_dd, icon_name="preferences-desktop-theme-symbolic", badge_class="badge-appearance")
        self.add_row(card_theme, "Dock Opacity", "Adjust background transparency of the dock (0.0 = clear glass, 1.0 = solid).", self.scale("dock", "opacity", 0.0, 1.0, 0.05, 2), icon_name="preferences-color-symbolic", badge_class="badge-appearance")

        card = self.card(box, "Behaviour")
        self.add_row(card, "Show the Dock", "Display the application dock along the bottom edge.", self.switch("dock", "enabled"), icon_name="view-paged-symbolic", badge_class="badge-dock")
        self.add_row(card, "Frosted Glass Blur", "Enable frosted glass blur effect under the dock.", self.switch("blur", "dock"), icon_name="preferences-desktop-wallpaper-symbolic", badge_class="badge-appearance")
        self.add_row(card, "Blur Strength", "Adjust the intensity and diffusion radius of the frosted glass blur.", self.scale("blur", "strength", 0.2, 2.5, 0.1, 1), icon_name="preferences-desktop-theme-symbolic", badge_class="badge-appearance")
        self.add_row(card, "Automatically Hide", "Hide the dock until the pointer reaches the bottom edge.", self.switch("dock", "autohide"), icon_name="view-reveal-symbolic", badge_class="badge-wifi")
        self.add_row(card, "Intelligent Autohide", "Hide the dock only when a window overlaps it.", self.switch("dock", "intellihide"), icon_name="preferences-system-windows-symbolic", badge_class="badge-workspaces")

        card_mag = self.card(box, "Size & Magnification")
        self.add_row(card_mag, "Icon Size", "Base icon size in pixels (32–96).", self.scale("dock", "icon_size", 32, 96, 1, 0))
        self.add_row(card_mag, "Magnification", "Grow icons under the pointer like macOS.", self.switch("dock", "magnification"))
        self.add_row(card_mag, "Maximum Scale", "How large an icon grows under the pointer.", self.scale("dock", "max_scale", 1.0, 2.2, 0.05, 2))
        self.add_row(card_mag, "Magnification Range", "Radius of the magnification hump in pixels.", self.scale("dock", "mag_radius", 40, 300, 5, 0))

        # Dynamic Island / Music Pill
        card_music = self.card(box, "Music Pill (Dynamic Island)")
        if "music_pill" not in self.cfg:
            self.cfg["music_pill"] = {"enabled": True, "position": "topbar_center"}

        sw_mp = Gtk.Switch()
        sw_mp.set_active(bool(self.cfg["music_pill"].get("enabled", True)))
        sw_mp.connect("notify::active", lambda s, _: self.on_music_pill_switch(s.get_active()))
        self.add_row(card_music, "Show Music Pill", "Display an interactive dynamic island music pill when media is playing.", sw_mp, icon_name="audio-x-generic-symbolic", badge_class="badge-sound")

        pos_labels = ["Top Bar (Center)", "Dock (Left)", "Dock (Right)", "Disabled"]
        pos_values = ["topbar_center", "dock_left", "dock_right", "disabled"]
        cur_pos = self.cfg["music_pill"].get("position", "topbar_center")
        pos_idx = pos_values.index(cur_pos) if cur_pos in pos_values else 0
        pos_dd = Gtk.DropDown.new_from_strings(pos_labels)
        pos_dd.set_selected(pos_idx)
        pos_dd.connect("notify::selected", lambda dd, _: self.on_music_pill_pos(pos_values[dd.get_selected()]))
        self.add_row(card_music, "Pill Placement", "Choose where the Dynamic Island appears (Top Bar Center, Dock Left or Right).", pos_dd, icon_name="preferences-system-symbolic", badge_class="badge-appearance")

        return box

    def on_music_pill_switch(self, active):
        if "music_pill" not in self.cfg:
            self.cfg["music_pill"] = {}
        self.cfg["music_pill"]["enabled"] = bool(active)
        self.changed()

    def on_music_pill_pos(self, pos_str):
        if "music_pill" not in self.cfg:
            self.cfg["music_pill"] = {}
        self.cfg["music_pill"]["position"] = pos_str
        self.changed()

    def on_dock_theme(self, theme_str):
        if "dock" not in self.cfg:
            self.cfg["dock"] = {}
        self.cfg["dock"]["theme"] = theme_str
        self.changed()

    # =======================================================================
    # PAGE: GESTURES
    # =======================================================================

    def page_gestures(self):
        box = self.page("Gestures", "Map touchpad gestures to shell actions and tune physics feel.")

        for group_title, gestures in GESTURE_GROUPS:
            card = self.card(box, group_title)
            for key, label in gestures:
                self.add_row(
                    card,
                    label,
                    f"Action triggered by {group_title.lower()} {label.lower()}",
                    self.dropdown("gestures", key),
                    icon_name="input-touchpad-symbolic",
                    badge_class="badge-gestures",
                )

        card_feel = self.card(box, "Physics & Springs")
        for key, title, subtitle, lo, hi, step, digits in SLIDERS:
            self.add_row(card_feel, title, subtitle, self.scale("physics", key, lo, hi, step, digits))

        card_live = self.card(box, "Live Touchpad Telemetry")
        status = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=14)
        status.set_size_request(320, -1)

        self.fingers_label = Gtk.Label(label="Fingers: —")
        self.fingers_label.add_css_class("mono")
        self.gesture_label = Gtk.Label(label="Gesture: —")
        self.gesture_label.add_css_class("mono")
        self.progress = Gtk.ProgressBar()
        self.progress.set_hexpand(True)
        self.progress.set_valign(Gtk.Align.CENTER)
        status.append(self.fingers_label)
        status.append(self.gesture_label)
        status.append(self.progress)

        self.add_row(
            card_live,
            "Touchpad Stream",
            "Touch the pad with 3 or 4 fingers to inspect live gesture recognition.",
            status,
            icon_name="utilities-system-monitor-symbolic",
            badge_class="badge-display",
        )
        return box

    # =======================================================================
    # PAGE: ABOUT
    # =======================================================================

    def page_about(self):
        box = self.page("About This Computer", "AuraDE — a macOS Sonoma/Sequoia-inspired Wayland desktop environment.")

        hostname = socket.gethostname()
        kernel = platform.release()
        arch = platform.machine()

        # Hero Banner (macOS Sonoma About This Mac style)
        hero = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
        hero.add_css_class("hero-banner")

        hero_badge = make_badge("computer-symbolic", "badge-wifi", "row-badge")
        hero.append(hero_badge)

        hcol = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        hcol.set_hexpand(True)
        ht = Gtk.Label(label=f"{hostname} • AuraDE Desktop", xalign=0)
        ht.add_css_class("title")
        hs = Gtk.Label(label=f"Arch Linux  •  Wayland Native Compositor  •  Kernel {kernel}", xalign=0)
        hs.add_css_class("subtitle")
        hcol.append(ht)
        hcol.append(hs)
        hero.append(hcol)

        ver_pill = Gtk.Label(label="AuraDE 2.0")
        ver_pill.add_css_class("connected-pill")
        ver_pill.set_valign(Gtk.Align.CENTER)
        hero.append(ver_pill)
        box.append(hero)

        # System Hardware Info Card
        card_sys = self.card(box, "Hardware & System Specifications")

        cpu_model = "Unknown Processor"
        try:
            with open("/proc/cpuinfo", "r") as f:
                for line in f:
                    if "model name" in line:
                        cpu_model = line.split(":", 1)[1].strip()
                        break
        except Exception:
            pass

        ram_str = "Unknown RAM"
        try:
            with open("/proc/meminfo", "r") as f:
                for line in f:
                    if "MemTotal" in line:
                        kb = int(line.split()[1])
                        ram_str = f"{kb / (1024 * 1024):.1f} GB"
                        break
        except Exception:
            pass

        sys_rows = [
            ("Computer Name", hostname, "computer-symbolic", "badge-wifi"),
            ("Operating System", "Arch Linux", "applications-system-symbolic", "badge-display"),
            ("Desktop Environment", "AuraDE (Wayland Native)", "preferences-desktop-display-symbolic", "badge-workspaces"),
            ("Kernel Version", f"{kernel} ({arch})", "utilities-terminal-symbolic", "badge-keyboard"),
            ("Processor", cpu_model, "preferences-system-symbolic", "badge-dock"),
            ("Memory (RAM)", ram_str, "drive-harddisk-solidstate-symbolic", "badge-gestures"),
        ]
        for slbl, sval, sic, sbadge in sys_rows:
            vl = Gtk.Label(label=sval)
            vl.add_css_class("subtitle")
            self.add_row(card_sys, slbl, None, vl, icon_name=sic, badge_class=sbadge)

        # Configuration Card
        card_cfg = self.card(box, "Configuration & Diagnostics")
        path = Gtk.Label(label=CONFIG_PATH)
        path.add_css_class("mono")
        path.set_selectable(True)

        buttons = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        save = Gtk.Button(label="Save Configuration")
        save.add_css_class("btn-primary")
        save.connect("clicked", lambda *_: self.save_now())
        buttons.append(save)

        folder = Gtk.Button(label="Open Config Folder")
        folder.connect("clicked", self.on_open_folder)
        buttons.append(folder)

        reset = Gtk.Button(label="Reset to Defaults")
        reset.add_css_class("btn-destructive")
        reset.connect("clicked", self.on_reset)
        buttons.append(reset)

        self.add_row(card_cfg, "Configuration File", "JSON configuration live-reloaded by the AuraDE compositor.", path, icon_name="document-properties-symbolic", badge_class="badge-about")
        self.add_row(card_cfg, "Profile Actions", "Save, inspect, or reset all AuraDE settings to factory defaults.", buttons)

        return box

    def on_open_folder(self, _button):
        try:
            Gtk.show_uri(self.win, "file://" + os.path.dirname(CONFIG_PATH), Gdk.CURRENT_TIME)
        except Exception as exc:
            print("Could not open folder:", exc)

    def on_reset(self, _button):
        self.cfg = json.loads(json.dumps(DEFAULTS))
        self.save_now()
        self.loading = True
        self.build_ui()
        self.loading = False

    # -- Telemetry ---------------------------------------------------------

    def start_telemetry(self):
        self.telemetry = Telemetry(self.on_telemetry)
        self.telemetry.start()

    def on_telemetry(self, packet):
        if not self.win or not hasattr(self, "fingers_label"):
            return
        fingers = packet.get("fingers", 0)
        gesture = packet.get("gesture", "IDLE")
        progress = float(packet.get("progress", 0.0))
        self.fingers_label.set_text("Fingers: %d" % fingers)
        self.gesture_label.set_text("Gesture: %s" % gesture)
        self.progress.set_fraction(max(0.0, min(1.0, progress)))


if __name__ == "__main__":
    app = AuraSettings()
    raise SystemExit(app.run(None))
