#!/bin/bash
# Surface Laptop 2 OV9734 webcam — uninstaller
# Run as the regular user (no sudo needed for the user-service parts).
# Will prompt for sudo where required.
set -euo pipefail

RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'; NC='\033[0m'
info()  { echo -e "${GREEN}[-]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }

# ── User service ──────────────────────────────────────────────────────────────
info "Stopping and disabling camera-bridge.service..."
systemctl --user stop    camera-bridge.service 2>/dev/null || true
systemctl --user disable camera-bridge.service 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/camera-bridge.service"
systemctl --user daemon-reload 2>/dev/null || true
info "  Done"

# ── System components (require sudo) ──────────────────────────────────────────
info "Removing system components (sudo required)..."

sudo bash -s <<'SUDO_BLOCK'
set -euo pipefail
GREEN='\033[0;32m'; NC='\033[0m'
info() { echo -e "${GREEN}[-]${NC} $*"; }

# Bridge script
rm -rf /usr/local/share/surface-laptop-2-camera
info "  Bridge script removed"

# DKMS modules
for mod_ver in ov9734-surface/1.0 ipu-bridge-ov9734/1.0; do
    mod=${mod_ver%%/*}; ver=${mod_ver##*/}
    if dkms status "$mod/$ver" 2>/dev/null | grep -q installed; then
        dkms remove "$mod/$ver" --all
        info "  DKMS: $mod/$ver removed"
    fi
    rm -rf "/usr/src/$mod-$ver"
done

# udev
rm -f /etc/udev/rules.d/99-ov9734-rebind.rules
rm -f /usr/local/sbin/ipu3-camera-rebind.sh
udevadm control --reload-rules
info "  udev rule removed"

# modprobe config
rm -f /etc/modprobe.d/v4l2loopback.conf
info "  v4l2loopback config removed"
SUDO_BLOCK

echo
echo -e "${GREEN}Uninstall complete.${NC} Please reboot."
