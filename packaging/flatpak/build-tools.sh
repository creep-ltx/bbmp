#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build ostree, flatpak and flatpak-builder into ~/.local/opt/flatpak, without root, to
# make the player's Flatpak on a machine that has no flatpak installed. Needs the usual
# development libraries (glib, libarchive, gpgme, fuse3, curl, libxml2, json-glib,
# libseccomp, appstream, xmlb) and bubblewrap.
set -euo pipefail
PREFIX="${1:-$HOME/.local/opt/flatpak}"
SRC="$HOME/.local/src/flatpak-tools"
mkdir -p "$SRC"
get() {   # repo tag file
  [ -f "$SRC/$3" ] || gh release download "$2" -R "$1" -p "$3" -D "$SRC"
  tar -C "$SRC" -xf "$SRC/$3"
}
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export PATH="$PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}"

get ostreedev/ostree v2026.4 libostree-2026.4.tar.xz
( cd "$SRC/libostree-2026.4" && ./configure --prefix="$PREFIX" --with-curl --without-soup \
    --disable-gtk-doc --disable-man --with-builtin-grub2-mkconfig --without-selinux \
    --disable-rofiles-fuse --with-openssl=no --without-libsystemd \
    BASH_COMPLETIONSDIR="$PREFIX/share/bash-completion/completions" \
    --with-systemdsystemunitdir="$PREFIX/lib/systemd/system" --with-dracut=no \
    && make -j"$(nproc)" && make install )

get flatpak/flatpak 1.18.4 flatpak-1.18.4.tar.xz
[ -d "$SRC/flatpak-1.18.4/build" ] || meson setup "$SRC/flatpak-1.18.4/build" "$SRC/flatpak-1.18.4" \
  --prefix="$PREFIX" --libdir=lib -Dsystem_helper=disabled -Dgtkdoc=disabled -Dman=disabled \
  -Ddocbook_docs=disabled -Dtests=false -Dsystemd=disabled -Dselinux_module=disabled \
  -Dsystem_bubblewrap=bwrap -Dmalcontent=disabled -Dinstalled_tests=false \
  -Ddbus_config_dir="$PREFIX/share/dbus-1/system.d" -Ddbus_service_dir="$PREFIX/share/dbus-1/services" \
  -Dprofile_dir="$PREFIX/etc/profile.d" -Dgdm_env_file=false \
  -Dsystemdsystemunitdir="$PREFIX/lib/systemd/system" -Dsystemduserunitdir="$PREFIX/lib/systemd/user" \
  -Dsystemdsystemenvgendir="$PREFIX/lib/systemd/system-environment-generators" \
  -Dsystemduserenvgendir="$PREFIX/lib/systemd/user-environment-generators" \
  -Dtmpfilesdir="$PREFIX/lib/tmpfiles.d" -Dsysusersdir="$PREFIX/lib/sysusers.d" \
  -Dsystem_install_dir="$PREFIX/var/lib/flatpak" -Dsysconfdir="$PREFIX/etc" \
  -Dlocalstatedir="$PREFIX/var"
ninja -C "$SRC/flatpak-1.18.4/build" && ninja -C "$SRC/flatpak-1.18.4/build" install

get flatpak/flatpak-builder 1.4.12 flatpak-builder-1.4.12.tar.xz
[ -d "$SRC/flatpak-builder-1.4.12/build" ] || meson setup "$SRC/flatpak-builder-1.4.12/build" \
  "$SRC/flatpak-builder-1.4.12" --prefix="$PREFIX" --libdir=lib -Ddocs=disabled -Dtests=false
ninja -C "$SRC/flatpak-builder-1.4.12/build" && ninja -C "$SRC/flatpak-builder-1.4.12/build" install
echo "installed into $PREFIX/bin"
