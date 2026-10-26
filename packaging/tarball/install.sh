#!/usr/bin/env bash
set -e

if [ "$EUID" -ne 0 ]; then
    echo "Error: Please run as root (e.g., sudo ./install.sh)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Installing Grocycle binaries..."
install -m 755 "${SCRIPT_DIR}/bin/grocycle" /usr/local/bin/grocycle
install -m 755 "${SCRIPT_DIR}/bin/grocycled" /usr/local/bin/grocycled

echo "==> Setting up configuration and data directories..."
mkdir -p /etc/grocycle
mkdir -p /var/lib/grocycle
chmod 0750 /var/lib/grocycle

if [ ! -f /etc/grocycle/config.json ]; then
    install -m 644 "${SCRIPT_DIR}/configs/grocycle.conf.example" /etc/grocycle/config.json
    echo "Created default configuration at /etc/grocycle/config.json"
fi

if [ -d /etc/systemd/system ]; then
    echo "==> Installing systemd service unit..."
    install -m 644 "${SCRIPT_DIR}/configs/grocycle.service" /etc/systemd/system/grocycle.service
    systemctl daemon-reload
    systemctl enable --now grocycle.service
    echo "Grocycle service enabled and started."
fi

echo "==> Grocycle installation complete!"
echo "Check service status: grocycle service status"
echo "Launch TUI:           grocycle"
