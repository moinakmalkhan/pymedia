#!/usr/bin/env bash
# pymedia installer — detects your OS, installs FFmpeg dev libs, builds, and installs.
# Usage: ./install.sh

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[pymedia]${NC} $1"; }
warn()  { echo -e "${YELLOW}[pymedia]${NC} $1"; }
error() { echo -e "${RED}[pymedia]${NC} $1"; exit 1; }

# ── Detect OS ──
detect_os() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        OS_ID="$ID"
        OS_LIKE="$ID_LIKE"
    elif [ "$(uname)" = "Darwin" ]; then
        OS_ID="macos"
    else
        OS_ID="unknown"
    fi
}

# ── Install system dependencies ──
install_deps() {
    case "$OS_ID" in
        ubuntu|debian|pop|linuxmint|elementary)
            info "Detected $OS_ID — installing with apt..."
            sudo apt update -qq
            sudo apt install -y gcc pkg-config \
                libavformat-dev libavcodec-dev libavutil-dev \
                libswresample-dev libswscale-dev
            ;;
        fedora)
            info "Detected Fedora — installing with dnf..."
            sudo dnf install -y gcc pkg-config ffmpeg-free-devel \
                libavcodec-free-devel libavformat-free-devel \
                libavutil-free-devel libswresample-free-devel \
                libswscale-free-devel
            ;;
        centos|rhel|rocky|alma)
            info "Detected $OS_ID — installing with dnf..."
            sudo dnf install -y gcc pkg-config ffmpeg-devel
            ;;
        arch|manjaro|endeavouros)
            info "Detected $OS_ID — installing with pacman..."
            sudo pacman -S --noconfirm --needed gcc pkg-config ffmpeg
            ;;
        opensuse*|sles)
            info "Detected $OS_ID — installing with zypper..."
            sudo zypper install -y gcc pkg-config ffmpeg-devel
            ;;
        macos)
            info "Detected macOS — installing with Homebrew..."
            if ! command -v brew &> /dev/null; then
                error "Homebrew not found. Install it from https://brew.sh"
            fi
            brew install gcc pkg-config ffmpeg
            ;;
        *)
            # Try ID_LIKE as fallback
            if echo "$OS_LIKE" | grep -qw "debian"; then
                info "Detected Debian-based OS — installing with apt..."
                sudo apt update -qq
                sudo apt install -y gcc pkg-config \
                    libavformat-dev libavcodec-dev libavutil-dev \
                    libswresample-dev libswscale-dev
            elif echo "$OS_LIKE" | grep -qw "fedora"; then
                info "Detected Fedora-based OS — installing with dnf..."
                sudo dnf install -y gcc pkg-config ffmpeg-free-devel \
                    libavcodec-free-devel libavformat-free-devel \
                    libavutil-free-devel libswresample-free-devel \
                    libswscale-free-devel
            elif echo "$OS_LIKE" | grep -qw "arch"; then
                info "Detected Arch-based OS — installing with pacman..."
                sudo pacman -S --noconfirm --needed gcc pkg-config ffmpeg
            else
                error "Unsupported OS: $OS_ID. Please install FFmpeg dev libraries manually.\nSee README.md for instructions."
            fi
            ;;
    esac
}

# ── Verify deps ──
verify_deps() {
    if ! command -v gcc &> /dev/null; then
        error "gcc not found after install. Something went wrong."
    fi
    if ! command -v pkg-config &> /dev/null; then
        error "pkg-config not found after install. Something went wrong."
    fi
    if ! pkg-config --exists libavformat libavcodec libavutil libswresample libswscale 2>/dev/null; then
        error "FFmpeg dev libraries not found after install. Something went wrong."
    fi
    info "All system dependencies verified."
}

# ── Build and install ──
build_and_install() {
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    cd "$SCRIPT_DIR"

    info "Installing pymedia (compiles C library automatically)..."
    pip install .

    info "Verifying installation..."
    python -c "from pymedia import get_video_info; print('pymedia installed successfully!')"
}

# ── Main ──
main() {
    echo ""
    echo "  ╔═══════════════════════════════╗"
    echo "  ║  pymedia installer v0.1.0  ║"
    echo "  ╚═══════════════════════════════╝"
    echo ""

    detect_os
    info "Detected OS: $OS_ID"

    install_deps
    verify_deps
    build_and_install

    echo ""
    info "Done! You can now use pymedia:"
    echo "    python -c \"from pymedia import extract_audio\""
    echo ""
}

main
