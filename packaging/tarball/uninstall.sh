#!/usr/bin/env bash
set -e

if [ "$EUID" -ne 0 ]; then
    echo "Error: Please run as root (e.g., sudo ./uninstall.sh)" >&2
    exit 1
fi

echo "==> Stopping and disabling grocycle.service..."
if systemctl is-active --quiet grocycle.service 2>/dev/null; then
    systemctl stop grocycle.service || true
fi
if systemctl is-enabled --quiet grocycle.service 2>/dev/null; then
    systemctl disable grocycle.service || true
fi
rm -f /etc/systemd/system/grocycle.service
systemctl daemon-reload || true

echo "==> Removing Grocycle binaries..."
rm -f /usr/local/bin/grocycle
rm -f /usr/local/bin/grocycled

read -p "Do you also want to remove data and configuration (/var/lib/grocycle, /etc/grocycle)? [y/N] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    rm -rf /var/lib/grocycle
    rm -rf /etc/grocycle
    rm -f /var/log/grocycle.log
    echo "Removed data and configuration."
fi

echo "==> Grocycle uninstalled successfully."
