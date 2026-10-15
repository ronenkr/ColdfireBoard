#!/bin/sh
# Fetch Ubuntu's m68k cross compiler (GCC 11, supports -mcpu=52254) and unpack
# it into toolchain/ - no root needed. Tested on Ubuntu 22.04.
#   tools/setup_toolchain.sh && . toolchain/env.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$ROOT/toolchain
mkdir -p "$T/debs" "$T/bin"
cd "$T/debs"
apt-get download gcc-11-m68k-linux-gnu cpp-11-m68k-linux-gnu binutils-m68k-linux-gnu \
    libgcc-11-dev-m68k-cross gcc-11-m68k-linux-gnu-base
for d in *.deb; do dpkg-deb -x "$d" "$T/root"; done
rm -rf "$T/debs"
ln -sf ../root/usr/bin/m68k-linux-gnu-gcc-11 "$T/bin/m68k-linux-gnu-gcc"
cat > "$T/env.sh" <<ENV
# source this:  . toolchain/env.sh  (the Makefiles find the compiler without it)
TC=$T/root/usr
export LD_LIBRARY_PATH=\$TC/lib/x86_64-linux-gnu:\$LD_LIBRARY_PATH
export PATH=$T/bin:\$TC/bin:\$PATH
ENV
"$T/root/usr/bin/m68k-linux-gnu-gcc-11" --version | head -1
