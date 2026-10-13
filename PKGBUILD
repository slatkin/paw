# Maintainer: slatkin
pkgname=paw
pkgver=1.28.0
pkgrel=1
_tag="${pkgver}-paw.${pkgrel}"
pkgdesc="Wayland terminal emulator, a fork of foot, with the kitty graphics protocol"
arch=('x86_64')
url="https://github.com/slatkin/paw"
license=('MIT')
depends=('fcft' 'fontconfig' 'libpng' 'libutf8proc' 'libxkbcommon' 'ncurses' 'pixman' 'wayland' 'zlib')
makedepends=('meson' 'ninja' 'python' 'scdoc' 'tllist' 'wayland-protocols')
optdepends=('bash-completion: completion for positional arguments'
            'libnotify: desktop notifications'
            'xdg-utils: open URLs')
# paw installs the foot terminfo entry, which foot-terminfo also installs.
conflicts=('foot-terminfo')
source=("https://github.com/slatkin/paw/releases/download/v${_tag}/paw-${_tag}.tar.gz")
# aur.yml sets the checksum for each release.
sha256sums=('SKIP')

build() {
  cd "paw-${_tag}"
  meson setup build --prefix=/usr --buildtype=release
  meson compile -C build
}

package() {
  cd "paw-${_tag}"
  DESTDIR="${pkgdir}" meson install -C build --no-rebuild
  install -Dm644 LICENSE -t "${pkgdir}/usr/share/licenses/${pkgname}/"
}
