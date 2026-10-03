#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
version="${1:-$(sed -n 's/^project(ClearMic VERSION \([^ ]*\).*/\1/p' "$root_dir/CMakeLists.txt" | head -n1)}"
build_dir="${BUILD_DIR:-$root_dir/build/linux-package}"
staging_dir="${TMPDIR:-/tmp}/clearmic-package-${UID:-1000}"
package_root="$staging_dir/package-root"
package_name="clearmic_${version}_amd64"

if [[ -z "$version" ]]; then
  echo "Could not determine package version" >&2
  exit 1
fi

cmake -S "$root_dir" -B "$build_dir/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$build_dir/build" --parallel
mkdir -p "$build_dir" "$staging_dir"
rm -rf "$package_root"
mkdir -m 0755 "$package_root"
DESTDIR="$package_root" cmake --install "$build_dir/build"
install -D -m 0644 "$root_dir/packaging/linux/clearmic-cli.1" "$package_root/usr/share/man/man1/clearmic-cli.1"
install -D -m 0644 "$root_dir/packaging/linux/clearmic-cli.desktop" "$package_root/usr/share/applications/clearmic-cli.desktop"

mkdir -p "$package_root/DEBIAN"
chmod 0755 "$package_root/DEBIAN" "$package_root/usr" "$package_root/usr/bin" "$package_root/usr/share" \
  "$package_root/usr/share/man" "$package_root/usr/share/man/man1" "$package_root/usr/share/applications"
cat > "$package_root/DEBIAN/control" <<CONTROL
Package: clearmic
Version: $version
Section: sound
Priority: optional
Architecture: amd64
Maintainer: ClearMic contributors
Depends: libpipewire-0.3-0, libgtk-3-0, libgstreamer1.0-0, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, libc6, libstdc++6
Description: Local microphone enhancement tools
 ClearMic captures a PipeWire microphone, applies local noise reduction,
 and can publish the processed signal as a virtual microphone source.
 The GTK desktop panel includes local device selection and A/B sample playback.
CONTROL

mkdir -p "$build_dir/dist"
dpkg-deb --root-owner-group --build "$package_root" "$build_dir/dist/$package_name.deb"
echo "Built $build_dir/dist/$package_name.deb"
