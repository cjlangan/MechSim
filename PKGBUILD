# Maintainer: Connor Langan <connorjameslangan@gmail.com>
pkgname=mechsim
pkgver=1.1.0
pkgrel=1
pkgdesc="Mechanical keyboard sound simulator"
arch=('x86_64')
url="https://github.com/cjlangan/mechsim"

depends=(
  'json-c'
  'libpulse'
  'systemd'  # for libudev.so
)

makedepends=(
  'gcc'
  'make'
  'pkgconf'
  'libevdev'
  'libinput'
  'libsndfile'
)

license=('MIT') 
source=("https://github.com/cjlangan/mechsim/releases/download/v${pkgver}/mechsim-v${pkgver}.tar.gz")
sha256sums=('d41d8cd98f00b204e9800998ecf8427e')

build() {
  cd "$srcdir/mechsim-v${pkgver}"
  make PREFIX=/usr
}

package() {
  cd "$srcdir/mechsim-v${pkgver}"
  make DESTDIR="$pkgdir" PREFIX=/usr install
}
