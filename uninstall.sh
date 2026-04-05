#!/bin/bash
# Surface Laptop 2 camera bridge — uninstaller
# No sudo required — everything is under ~/.local and ~/.config.
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
info()  { echo -e "${GREEN}[-]${NC} $*"; }

# ── User service ──────────────────────────────────────────────────────────────
info "Stopping and disabling camera-bridge.service..."
systemctl --user stop    camera-bridge.service 2>/dev/null || true
systemctl --user disable camera-bridge.service 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/camera-bridge.service"
systemctl --user daemon-reload 2>/dev/null || true
info "  Done"

# ── GNOME extension ──────────────────────────────────────────────────────────
info "Removing GNOME extension..."
EXT_UUID="camera-bridge@surface"
gnome-extensions disable "$EXT_UUID" 2>/dev/null || true
rm -rf "$HOME/.local/share/gnome-shell/extensions/$EXT_UUID"
info "  Done"

# ── Bridge binary ─────────────────────────────────────────────────────────────
info "Removing camera bridge binary..."
rm -f "$HOME/.local/bin/camera-bridge"
info "  Done"

echo
echo -e "${GREEN}Uninstall complete.${NC}"
