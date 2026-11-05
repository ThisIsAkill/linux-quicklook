#!/bin/bash
# Builds quicklook, installs it to ~/.local, and sets up Space-to-preview
# for Dolphin. Run ./install.sh --uninstall to undo everything it set up.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PREFIX="$HOME/.local"
UNIT_DIR="$HOME/.config/systemd/user"
ENV_FILE="$HOME/.config/environment.d/quicklook.conf"
# Earlier versions set the accessibility flag for Dolphin only, with these:
OLD_DOLPHIN_DESKTOP="$HOME/.local/share/applications/org.kde.dolphin.desktop"
OLD_DOLPHIN_DROPIN="$UNIT_DIR/plasma-dolphin.service.d/quicklook.conf"
MARKER="# Added by quicklook"

uninstall() {
    systemctl --user disable --now quicklook.service 2>/dev/null || true
    rm -f "$UNIT_DIR/quicklook.service" "$PREFIX/bin/quicklook"
    rm -f "$ENV_FILE"
    remove_old_dolphin_setup
    systemctl --user daemon-reload
    echo "quicklook removed. The accessibility setting goes away at your next login."
}

remove_old_dolphin_setup() {
    rm -f "$OLD_DOLPHIN_DROPIN"
    rmdir "$UNIT_DIR/plasma-dolphin.service.d" 2>/dev/null || true
    if [ -f "$OLD_DOLPHIN_DESKTOP" ] && grep -q "^$MARKER" "$OLD_DOLPHIN_DESKTOP"; then
        rm -f "$OLD_DOLPHIN_DESKTOP"
    fi
}

if [ "${1:-}" = "--uninstall" ]; then
    uninstall
    exit 0
fi

echo "==> Building"
cmake -S "$HERE" -B "$HERE/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build "$HERE/build"
cmake --install "$HERE/build" >/dev/null

echo "==> Letting Dolphin report the focused file (Qt accessibility, from your next login)"
# Read by systemd and Plasma at login, so every Dolphin gets it: launched
# from the menu, by another app, or restored from the last session. It only
# affects Qt/KDE apps, which then answer accessibility queries.
mkdir -p "$(dirname "$ENV_FILE")"
printf '%s\nQT_LINUX_ACCESSIBILITY_ALWAYS_ON=1\n' "$MARKER" > "$ENV_FILE"
remove_old_dolphin_setup

echo "==> Starting the background service"
mkdir -p "$UNIT_DIR"
cat > "$UNIT_DIR/quicklook.service" <<EOF
[Unit]
Description=Quick Look: press Space in Dolphin to preview files
PartOf=graphical-session.target
After=graphical-session.target

[Service]
ExecStart=$PREFIX/bin/quicklook --daemon
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
EOF
systemctl --user daemon-reload
systemctl --user enable quicklook.service >/dev/null 2>&1
if [ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]; then
    systemctl --user restart quicklook.service
    echo "    running."
else
    echo "    enabled; it starts with your next Plasma login."
fi

cat <<EOF

Done. Log out of Plasma and back in once (so apps start with the
accessibility setting), then select a file in Dolphin and press Space.

  Space        open / close the preview
  Arrow keys   move through files in Dolphin; the preview follows
  Esc          close the preview

You can also preview from a terminal:  quicklook FILE
EOF
