# Surface Laptop 2 — Camera Bridge

Userspace camera bridge for the **Surface Laptop 2 (Model 1769)** running
the [linux-surface](https://github.com/linux-surface/linux-surface) kernel on
Fedora Silverblue, with [tomgood18](https://github.com/tomgood18/surface-laptop-2-camera) drivers installed.

Reads raw IPU3 Bayer frames from the OV9734 sensor, debayers them to RGB, and
publishes a **PipeWire Video/Source** node plus a V4L2 loopback device. The
camera appears as **"Surface Laptop 2 Webcam"** in any application.

To avoid high idle CPU usage, A **GNOME Shell extension** provides a top-bar toggle to start/stop the bridge
and adjust camera brightness via D-Bus — no always-on service required.

> **Note:** This repo only handles the userspace bridge. Kernel-level fixes
> (DKMS modules for `ipu_bridge` and `ov9734`, udev rebind rules) are managed
> separately via the Silverblue image.

---

## Requirements

| Requirement | Notes |
|-------------|-------|
| Hardware | Surface Laptop 2 (Model 1769) |
| Kernel | [linux-surface](https://github.com/linux-surface/linux-surface) kernel |
| Driver | [tomgood18](https://github.com/tomgood18/surface-laptop-2-camera) IPU3/OV9734 DKMS modules |
| Desktop | GNOME Shell 49+ on PipeWire |
| Build tools | `g++`, `pkg-config`, GStreamer 1.0 dev headers, GLib 2.0 dev headers |

---

## How it works

The IPU3 CIO2 captures raw `SGRBG10_1X10` (Bayer 10-bit) frames. No in-kernel
debayer path exists for this format, so the bridge:

1. Configures the media pipeline (`media-ctl`) and sensor controls
2. Reads raw frames via `v4l2-ctl --stream-mmap`
3. Unpacks IPU3 RAW10 → 8-bit Bayer
4. Debayers with 2x2 binning → 648x367 RGB
5. Pushes frames to a **PipeWire Video/Source** node via GStreamer `pipewiresink`
6. Simultaneously writes YUYV to `/dev/video20` (V4L2 loopback) for apps that use V4L2 directly

The bridge watches for consumer connections (PipeWire links or direct V4L2
readers) and only starts the sensor when an application requests the camera.
It stops automatically ~5 seconds after the last consumer disconnects.

### Brightness control

The bridge exposes a D-Bus interface (`com.surface.CameraBridge`) for
adjusting brightness at runtime. The GNOME extension uses this to provide
up/down controls in the panel menu.

---

## Installation

```bash
git clone https://github.com/bwagley/surface-laptop-2-camera.git
cd surface-laptop-2-camera
./install.sh
```

No `sudo` required — everything installs under `~/.local` and `~/.config`:

| Component | Path |
|-----------|------|
| Bridge binary | `~/.local/bin/camera-bridge` |
| Systemd user service | `~/.config/systemd/user/camera-bridge.service` |
| GNOME extension | `~/.local/share/gnome-shell/extensions/camera-bridge@surface/` |

After installing, log out and back in (or restart GNOME Shell) to load the
extension.

---

## Usage

1. A **camera icon** appears in the GNOME top bar
2. Click it to open the menu:
   - **Toggle switch** — start/stop the camera bridge service
   - **Brightness controls** — adjust camera brightness (0%–300%)
   - **Status** — shows whether the bridge is running
3. When the bridge is running, the camera appears in Cheese, Firefox, OBS, etc.

To start/stop manually without the extension:

```bash
systemctl --user start camera-bridge.service
systemctl --user stop camera-bridge.service
```

---

## Uninstalling

```bash
cd surface-laptop-2-camera
./uninstall.sh
```

---

## Troubleshooting

### Camera not found in apps

Check the bridge service:
```bash
systemctl --user status camera-bridge.service
journalctl --user -u camera-bridge.service -n 50
```

### "Could not find ipu3-cio2 1 video node"

The kernel-level DKMS modules or udev rebind may not be working. This is
handled outside this repo — check your Silverblue image configuration.

```bash
dmesg | grep -E "ov9734|ipu3|OVTI"
media-ctl -d /dev/media0 -p | grep -E "ov9734|cio2"
```

### Green / colour-shifted image

The debayer white balance is tuned in `camera-bridge.cpp` in `debayer_half()`.
Adjust the integer multipliers:
```cpp
r = (r * 13) >> 3;  // ~1.625 — increase to warm up
// g stays as-is (x1.0)
b = (b * 10) >> 3;  // ~1.25  — increase to cool down
```

Rebuild and reinstall after changes:
```bash
./install.sh
systemctl --user restart camera-bridge.service
```

---

## Building manually

```bash
cd bridge
make
```

Requires: `gstreamer1-devel`, `gstreamer1-plugins-base-devel`, `glib2-devel`,
`pkg-config`, `g++`.

---

## File layout

```
bridge/
  camera-bridge.cpp       C++ bridge source
  Makefile                Build configuration
  camera-bridge.service   Systemd user service unit
gnome-extension/
  camera-bridge@surface/
    extension.js          GNOME Shell extension (toggle + brightness)
    metadata.json         Extension metadata
    stylesheet.css        Extension styles
install.sh                Build and install everything
uninstall.sh              Remove everything
```

---

## Tested on

| Distribution | Kernel | Desktop | Status |
|--------------|--------|---------|--------|
| Fedora Silverblue 43 | 6.19.10-surface | GNOME 49 | Working |

---

## Acknowledgements

Forked from [tomgood19](https://github.com/tomgood18/surface-laptop-2-camera), this just rewrites the userspace handling for more efficient running. 
