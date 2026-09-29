#!/bin/sh
# Install PenguinCAD, and whatever it needs, on any Linux distribution:
#
#   curl -fsSL https://kaiyao7572-creator.github.io/PenguinCAD/install.sh | sh
#
# 1. Flatpak itself, from your distribution's packages, if it is missing
#    (the only step that asks for your password).
# 2. The Flathub repository, which carries the Qt runtime PenguinCAD runs on.
# 3. PenguinCAD, from its own signed repository, so it updates like any
#    other app.
#
# Everything after step 1 is per-user: nothing else touches the system.
# Running it again is harmless; it updates PenguinCAD if already installed.
set -eu

APP_ID=io.github.kaiyao7572_creator.PenguinCAD
REF_URL=https://kaiyao7572-creator.github.io/PenguinCAD/penguincad.flatpakref
FLATHUB=https://dl.flathub.org/repo/flathub.flatpakrepo

say() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Linux ] || fail "PenguinCAD runs on Linux only."
case "$(uname -m)" in
    x86_64 | amd64) ;;
    *) fail "PenguinCAD is only built for 64-bit PCs (x86-64) so far, not $(uname -m)." ;;
esac

# Root, or sudo when there is one; the package step needs it.
as_root() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        fail "Installing Flatpak needs root. Run as root, or install Flatpak yourself: https://flathub.org/setup"
    fi
}

# ---- 1. Flatpak ----
if command -v flatpak >/dev/null 2>&1; then
    say "Flatpak is already installed."
else
    say "Installing Flatpak (you may be asked for your password)..."
    if command -v dnf >/dev/null 2>&1; then
        as_root dnf install -y flatpak
    elif command -v apt-get >/dev/null 2>&1; then
        as_root apt-get update
        as_root apt-get install -y flatpak
    elif command -v pacman >/dev/null 2>&1; then
        as_root pacman -S --needed --noconfirm flatpak
    elif command -v zypper >/dev/null 2>&1; then
        as_root zypper --non-interactive install flatpak
    elif command -v eopkg >/dev/null 2>&1; then
        as_root eopkg install -y flatpak
    elif command -v apk >/dev/null 2>&1; then
        as_root apk add flatpak
    elif command -v xbps-install >/dev/null 2>&1; then
        as_root xbps-install -Sy flatpak
    else
        fail "Could not find a package manager to install Flatpak with. Install it by hand (https://flathub.org/setup), then run this again."
    fi
    FRESH_FLATPAK=1
fi

# ---- 2. Flathub, for the Qt runtime ----
say "Making sure Flathub is available..."
flatpak remote-add --user --if-not-exists flathub "$FLATHUB"

# ---- 3. PenguinCAD ----
if flatpak info --user "$APP_ID" >/dev/null 2>&1; then
    say "PenguinCAD is already installed; updating it..."
    flatpak update --user -y --noninteractive "$APP_ID"
else
    say "Installing PenguinCAD and the runtime it needs (a few hundred MB the first time)..."
    flatpak install --user -y --noninteractive "$REF_URL"
fi

say "Done. Start PenguinCAD from your applications menu, or run:"
printf '    flatpak run %s\n' "$APP_ID"
if [ "${FRESH_FLATPAK:-0}" = 1 ]; then
    printf '\nFlatpak was just installed: log out and back in once, so PenguinCAD\nappears in your applications menu.\n'
fi
