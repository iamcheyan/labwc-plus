#!/bin/bash
set -e

# Resolve to repository root
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${SRC_DIR}"

PREFIX="/opt/labwc-dev"
BUILDDIR="builddir-dev"

# ── Detect distro ──────────────────────────────────────
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        case "$ID" in
            ubuntu|debian|pop|linuxmint) echo "debian" ;;
            fedora|rhel|centos|rocky|alma) echo "fedora" ;;
            opensuse*|suse*) echo "suse" ;;
            arch|manjaro|endeavouros) echo "arch" ;;
            *) echo "unknown" ;;
        esac
    else
        echo "unknown"
    fi
}

DISTRO=$(detect_distro)
echo "==> Detected distro family: ${DISTRO}"

# ── Install dependencies ───────────────────────────────
install_deps() {
    echo "==> Installing build dependencies..."
    case "$DISTRO" in
        debian)
            sudo apt update
            sudo apt install -y \
                meson ninja-build gcc pkg-config \
                libwayland-dev wayland-protocols \
                libwlroots-dev wlroots-0.20-tools \
                libxml2-dev libcairo2-dev libpango1.0-dev \
                libglib2.0-dev libinput-dev libudev-dev \
                libpixman-1-dev libpng-dev libdrm-dev \
                libxkbcommon-dev librsvg2-dev \
                libsfdo-basedir-dev libsfdo-desktop-dev libsfdo-icon-dev \
                hwdata
            ;;
        fedora)
            sudo dnf install -y \
                meson gcc pkg-config \
                wayland-devel wayland-protocols-devel \
                wlroots-devel \
                libxml2-devel cairo-devel pango-devel \
                glib2-devel libinput-devel systemd-devel \
                pixman-devel libpng-devel libdrm-devel \
                libxkbcommon-devel librsvg2-devel \
                libsfdo-devel \
                hwdata
            ;;
        arch)
            sudo pacman -S --needed --noconfirm \
                meson gcc pkg-config \
                wayland wayland-protocols \
                wlroots \
                libxml2 cairo pango \
                glib2 libinput systemd \
                pixman libpng libdrm \
                libxkbcommon librsvg \
                libsfdo \
                hwdata
            ;;
        *)
            echo "ERROR: Unsupported distro. Install dependencies manually:"
            echo "  meson, ninja, gcc, pkg-config, wayland-dev, wlroots-dev,"
            echo "  libxml2-dev, cairo-dev, pango-dev, glib2-dev, libinput-dev,"
            echo "  pixman-dev, libpng-dev, libdrm-dev, libxkbcommon-dev,"
            echo "  librsvg2-dev, libsfdo-dev, hwdata"
            exit 1
            ;;
    esac
}

# Check if meson is available, if not install deps
if ! command -v meson &>/dev/null; then
    install_deps
fi

# ── Setup builddir if needed ───────────────────────────
if [ ! -d "${BUILDDIR}" ]; then
    echo "==> Setting up meson build directory..."
    # Try with xwayland, fall back to disabled
    if meson setup "${BUILDDIR}" --prefix="${PREFIX}" -Dxwayland=enabled 2>/dev/null; then
        echo "    xwayland: enabled"
    else
        meson setup "${BUILDDIR}" --prefix="${PREFIX}" -Dxwayland=disabled
        echo "    xwayland: disabled"
    fi
fi

# ── Build ──────────────────────────────────────────────
echo "==> Compiling labwc..."
ninja -C "${BUILDDIR}"

# ── Install ────────────────────────────────────────────
echo "==> Installing to ${PREFIX}..."
sudo ninja -C "${BUILDDIR}" install

# ── Symlink for GDM ────────────────────────────────────
echo "==> Creating /usr/local/bin/labwc-dev symlink..."
sudo ln -sf "${PREFIX}/bin/labwc" /usr/local/bin/labwc-dev

# ── GDM session file ───────────────────────────────────
SESSION_FILE="/usr/share/wayland-sessions/labwc-dev.desktop"
if [ ! -f "${SESSION_FILE}" ]; then
    echo "==> Creating GDM session file..."
    sudo tee "${SESSION_FILE}" > /dev/null << EOF
[Desktop Entry]
Name=labwc (dev)
Comment=labwc development build
Exec=/usr/local/bin/labwc-dev
Type=Application
DesktopNames=labwc
EOF
fi

echo ""
echo "==> Done! labwc-dev installed to ${PREFIX}"
echo "    Binary:  ${PREFIX}/bin/labwc"
echo "    Symlink: /usr/local/bin/labwc-dev"
echo ""
echo "    Test:  GDM → select 'labwc (dev)' session"
echo "    Or:    WLR_RENDERER=pixman /opt/labwc-dev/bin/labwc"
