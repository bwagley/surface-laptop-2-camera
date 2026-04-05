#!/bin/bash
# Surface Laptop 2 camera bridge installer
# Builds and installs the userspace camera bridge, systemd service,
# and GNOME shell extension.
#
# No sudo required — everything installs under ~/.local and ~/.config.
set -euo pipefail

# ── Helpers ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[✗]${NC} $*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Build camera bridge binary ────────────────────────────────────────────────
info "Building camera bridge (C++)..."
make -C "$SCRIPT_DIR/bridge" clean
make -C "$SCRIPT_DIR/bridge"
info "  camera-bridge built"

# ── Install camera bridge binary ──────────────────────────────────────────────
info "Installing camera bridge binary..."
install -Dm755 "$SCRIPT_DIR/bridge/camera-bridge" "$HOME/.local/bin/camera-bridge"
info "  ~/.local/bin/camera-bridge installed"

# ── User systemd service ──────────────────────────────────────────────────────
info "Installing user systemd service..."
SERVICE_DIR="$HOME/.config/systemd/user"
mkdir -p "$SERVICE_DIR"
install -m 644 "$SCRIPT_DIR/bridge/camera-bridge.service" "$SERVICE_DIR/"
systemctl --user daemon-reload
info "  camera-bridge.service installed (start/stop via GNOME extension)"

# ── GNOME extension ──────────────────────────────────────────────────────────
info "Installing GNOME extension..."
EXT_UUID="camera-bridge@surface"
EXT_DIR="$HOME/.local/share/gnome-shell/extensions/$EXT_UUID"
mkdir -p "$EXT_DIR"
install -m 644 "$SCRIPT_DIR/gnome-extension/$EXT_UUID/extension.js"  "$EXT_DIR/"
install -m 644 "$SCRIPT_DIR/gnome-extension/$EXT_UUID/metadata.json" "$EXT_DIR/"
install -m 644 "$SCRIPT_DIR/gnome-extension/$EXT_UUID/stylesheet.css" "$EXT_DIR/"
info "  GNOME extension installed to $EXT_DIR"

# Try to enable the extension
if gnome-extensions enable "$EXT_UUID" 2>/dev/null; then
    info "  Extension enabled"
else
    warn "  Could not enable extension automatically. After logging in, run:"
    warn "    gnome-extensions enable $EXT_UUID"
fi

# ── Done ──────────────────────────────────────────────────────────────────────
echo
echo -e "${GREEN}Installation complete!${NC}"
echo
echo "  The 'Camera Bridge' GNOME extension adds a camera icon to the top bar."
echo "  Click it to toggle the camera bridge service on/off and adjust brightness."
echo
echo "  To start the camera bridge manually:"
echo "    systemctl --user start camera-bridge.service"
echo
