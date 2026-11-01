# AuraDE — Fluid macOS-Logic Wayland Desktop Environment

[![C99](https://img.shields.io/badge/Language-C99-00599C.svg)](https://en.wikipedia.org/wiki/C99)
[![Wayland](https://img.shields.io/badge/Display%20Server-Wayland-FF7F00.svg)](https://wayland.freedesktop.org/)
[![wlroots](https://img.shields.io/badge/wlroots-0.20-555555.svg)](https://gitlab.freedesktop.org/wlroots/wlroots)
[![GTK4](https://img.shields.io/badge/GUI-GTK4%20%2B%20Python-4B8BBE.svg)](https://www.gtk.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

**AuraDE** is a modern, lightweight, native Wayland desktop environment for Linux. Engineered from the ground up in pure C99 and wlroots, AuraDE combines the direct-manipulation responsiveness, continuous multi-finger trackpad physics, and elegant aesthetic of macOS with the speed and freedom of Linux.

---

## 🌟 Key Highlights

- **1:1 Physics-Driven Gestures**: Continuous scrubbing with zero latency. Move fingers 1 mm and the visual canvas follows in lockstep. Features real velocity-inheriting spring differential equations and non-linear rubber-banding.
- **Minimalist Sun Menu Bar**: Clean, unified top menu bar with an original minimalist sun system menu, active application title, system clock, and integrated Control Center pill.
- **macOS-Style Modular Control Center**: Single-click access to Wi-Fi, Bluetooth, volume, display brightness, Night Light, Dark Mode, and media controls.
- **Liquid Dock**: Parabolic icon magnification, intelligent autohide (dodges overlapping windows), running indicator dots, drag-to-reorder pinned applications, and right-click context menu (New Window, Unpin / Keep in Dock, Quit).
- **Fullscreen Launchpad**: Frosted-glass application launcher with custom folders, drag-and-drop icon reorganization, real-time search, multi-page swipe transitions, and drag-to-pin directly to the dock.
- **Native GTK4 Settings Center**: Comprehensive control panel covering Wi-Fi, Bluetooth, Audio, Displays, Dynamic Workspaces, Mouse/Touchpad, Keyboard layouts, Appearance, Dock, and Gestures.
- **Server-Side Cursor Architecture**: Native `wp_cursor_shape_manager_v1` protocol support paired with the custom `Aura-Circle` concentric circular cursor theme across all Wayland and GTK apps.
- **macOS-Style Notification Banners**: Native `org.freedesktop.Notifications` D-Bus daemon supporting Telegram, browsers, and desktop apps with frosted-glass cards, slide-in animations, app avatar badges, custom Telegram vector icon artwork, audio alerts, and interactive click-to-focus.

---

## 🖐️ Gesture Mechanics (macOS Standard)

Unlike traditional Linux window managers that treat gestures as binary triggers after a delay, AuraDE tracks touch contacts continuously from hardware `evdev` multi-touch slots.

| Gesture | Action | Visual Behavior |
| :--- | :--- | :--- |
| **3 or 4-Finger Swipe Up** | **Mission Control** | Windows scale down smoothly into an organized, non-overlapping grid layout; top Spaces bar slides into view. |
| **3 or 4-Finger Swipe Down** | **App Exposé** | Filters and spreads out all windows belonging to the focused application, dimming background apps. |
| **3 or 4-Finger Swipe Left / Right** | **Spaces (Workspaces)** | Horizontal carousel tracks fingers 1:1 with velocity inheritance and elastic edge rubber-banding. |
| **4-Finger Pinch In / Out** | **Launchpad** | Open windows scale back as a frosted-glass application launcher zooms in. Repeating the gesture closes it. |
| **3-Finger Pinch Out / Spread** | **Show Desktop** | Windows scatter radially toward display edges, revealing the wallpaper. |
| **Mouse Hover on Dock** | **Parabolic Magnification** | Icons expand smoothly under the cursor using cosine-squared parabolic curves. |

### Physics Engine
- **Spring Equation**: Lift-off velocity seeds a damped harmonic oscillator:
  $$\ddot{x} + 2\zeta\omega_0 \dot{x} + \omega_0^2(x - x_{\text{target}}) = 0$$
- **Rubber-Banding**: Swiping past the carousel limits applies non-linear resistance:
  $$x_{\text{display}} = \frac{x}{1 + \frac{|x|}{K}}$$
- **Hardware Precision**: Normalizes raw touchpad coordinates to 1920×1080 reference pixels, rejecting resting jitter, palms (5+ contacts), and firmware touch jumps.

---

## 🎛️ Settings Application (`aurade-settings`)

AuraDE includes a dedicated, responsive GTK4 settings suite (`settings/aurade_settings.py`):

| Module | Features & Capabilities |
| :--- | :--- |
| **Wi-Fi** | Radio power toggle, live connected network stats (SSID, IP, gateway), available network scan, signal quality, and password authentication dialog. |
| **Bluetooth** | Power and discoverability toggles, paired accessories management (connect/disconnect/remove), and live device discovery scanning. |
| **Sound** | Master output volume slider, mute toggle, audio sink selector, channel sound test (`pw-play`), and microphone input gain controls. |
| **Displays** | Panel backlighting slider (`brightnessctl`), resolution mode dropdown, Night Light warm-temperature controls, and UI scale presets. |
| **Workspaces** | Dynamic workspace management (1 to 16 spaces), add or delete spaces, visual desktop overview cards, real-time inotify compositor synchronization, and window auto-reparenting. |
| **Mouse & Touchpad** | Tap-to-click, tracking speed, natural scrolling (macOS direction), palm rejection (disable-while-typing), and acceleration curves. |
| **Keyboard** | Multiple XKB layout strings (e.g. `us,ua`), repeat delay/speed sliders, and shortcut reference cheatsheet. |
| **Appearance** | Dark/Light glass theme switch, custom accent colour picker, cursor theme & size selector, and wallpaper chooser. |
| **Dock & Gestures** | Dock autohide/intellihide, icon sizing, magnification range, 3/4-finger gesture mappings, and live touchpad telemetry. |
| **About** | Hardware specs (CPU, RAM, GPU, Kernel, Hostname) and direct config backup/reset controls. |

---

## 📦 Prerequisites & Installation

### Arch Linux / Manjaro
```bash
sudo pacman -S gcc make pkgconf wlroots wayland wayland-protocols \
               libxkbcommon cairo pango json-c libinput libdrm \
               librsvg python python-gobject gtk4 pipewire wireplumber \
               networkmanager bluez bluez-utils brightnessctl
```

### Fedora (40+)
```bash
sudo dnf install gcc make pkgconfig wlroots-devel wayland-devel \
                 wayland-protocols-devel libxkbcommon-devel cairo-devel \
                 pango-devel json-c-devel libinput-devel libdrm-devel \
                 librsvg2-devel python3 python3-gobject gtk4 \
                 pipewire wireplumber NetworkManager bluez brightnessctl
```

### Ubuntu / Debian (24.04+)
```bash
sudo apt install build-essential pkg-config libwlroots-dev libwayland-dev \
                 wayland-protocols libxkbcommon-dev libcairo2-dev \
                 libpango1.0-dev libjson-c-dev libinput-dev libdrm-dev \
                 librsvg2-dev python3 python3-gi gir1.2-gtk-4.0 \
                 pipewire wireplumber network-manager bluez brightnessctl
```

---

## 🛠️ Building & Testing

### 1. Build the Compositor
```bash
git clone https://github.com/romanykh/aurade.git
cd aurade
make -j$(nproc)
```

### 2. Run Deterministic Gesture Tests
Run the standalone gesture simulation suite (requires no hardware touchpad):
```bash
make test-gestures
```

### 3. Test Nested in an Existing Session
To run AuraDE inside your current Wayland or X11 desktop inside a window:
```bash
./test_nested.sh
```

### 4. Install Session for Display Managers (GDM / SDDM)
To log directly into AuraDE from your display manager:
```bash
sudo cp session/aurade.desktop /usr/share/wayland-sessions/
```
Log out and choose **AuraDE** from your login screen.

---

## 📁 Source Tree

```
aurade/
├── src/
│   ├── main.c                  # Wayland display server, event loop, seats & inputs
│   ├── server.h                # Server state, view definitions & protocols
│   ├── gestures/
│   │   ├── gesture_engine.c    # Multi-touch evdev hardware gesture tracker
│   │   ├── gesture_engine.h
│   │   ├── physics.c           # Closed-form ODE spring simulation & rubber-banding
│   │   └── physics.h
│   ├── shell/
│   │   ├── topbar.c            # Minimalist sun menu bar, title, clock, control center
│   │   ├── dock.c              # Magnified bottom dock with intellihide
│   │   ├── mission_control.c   # Multi-window Mission Control grid
│   │   ├── launchpad.c         # Fullscreen application launcher with live search
│   │   ├── control_center.c    # Modular Control Center (Wi-Fi, BT, audio, brightness)
│   │   ├── screenshot.c        # Interactive region & fullscreen screenshot overlay
│   │   ├── notifications.c     # macOS-style desktop notifications D-Bus daemon
│   │   └── wallpaper.c         # Procedural dynamic gradient desktop background
│   ├── window/
│   │   ├── xdg_shell.c         # Window management and XDG surface lifecycle
│   │   └── workspace.c         # Multi-workspace carousel & scene translation
│   ├── render/
│   │   └── cairo_buffer.c      # Cairo vector drawing and status icon rendering
│   ├── config/
│   │   └── config.c            # Hot-reloading JSON configuration loader
│   └── util/
│       └── apps.c              # FreeDesktop .desktop application scanner & launcher
├── settings/
│   ├── aurade_settings.py      # Full GTK4 Settings application
│   └── aurade-settings.desktop # Application desktop entry
├── session/
│   ├── aurade-session          # Wayland session startup script & environment setup
│   └── aurade.desktop          # Display manager entry
├── tools/
│   ├── build_cursor_theme.py   # Multi-resolution Aura-Circle cursor generator
│   └── gesture_sim.c           # Synthetic gesture tests
└── Makefile                    # Modular build system
```

---

## ⚖️ Legal & License

- **Source Code**: Released under the [MIT License](LICENSE).
- **Clean-Room Implementation**: AuraDE is an independent, clean-room software project built with standard open-source libraries. Functional desktop interactions, windowing systems, and gesture mechanics are not subject to copyright.
- **Trademarks**: AuraDE uses its own branding and an original minimalist Sun logo. It does not contain proprietary Apple graphics, trademarks, or copyrighted assets.
