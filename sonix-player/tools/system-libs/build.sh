#!/bin/bash
#
# build.sh STOCK_ROOTFS OUT_DIR
#
# Builds the system libraries and programs that sonix-packer/assets puts in
# place of the stock ones, for the device's glibc 2.22:
#
#   usr/lib/libssl.so.1.1, libcrypto.so.1.1   OpenSSL 1.1.1w
#   usr/lib/libupnp.so.6, libixml.so.2,
#           libthreadutil.so.6                 pupnp 1.6.25
#   usr/lib/libsndfile.so.1                    libsndfile 1.2.2
#   usr/lib/libfdk-aac.so.2                    fdk-aac 2.0.3
#   usr/bin/ntfs-3g                            ntfs-3g 2022.10.3, libntfs-3g linked in
#   usr/bin/thttpd                             thttpd 2.29
#
# Same sonames, paths and configuration as the stock builds, so the stock
# programs that use them (wpa_supplicant, ntpd, curl, adbd, dmrd, bluealsa)
# load them unchanged.
#
# STOCK_ROOTFS is an extracted stock root filesystem; its lib/ and
# usr/lib/libfuse.so.2 are linked against, so nothing asks for a symbol or a
# symbol version the device does not have. abicheck.py checks every result
# against it at the end.
#
# Needs an Ubuntu 24.04 (or Debian) host with:
#   gcc-9-mipsel-linux-gnu binutils-mipsel-linux-gnu libc6-dev-mipsel-cross
#   autoconf automake libtool pkg-config make git perl python3-pyelftools
#   zlib1g-dev (for zlib.h)
# WORK (default ./system-libs-work) holds the sources and the build trees.

set -euo pipefail

if [ $# -ne 2 ]; then
	sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOTFS="$(cd "$1" && pwd)"
mkdir -p "$2"
OUT="$(cd "$2" && pwd)"
WORK="${WORK:-$PWD/system-libs-work}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
JOBS="${JOBS:-$(nproc)}"

CROSS=mipsel-linux-gnu
GCC=$CROSS-gcc-9

for tool in $GCC $CROSS-strip $CROSS-ar autoreconf libtoolize make git pkg-config perl python3; do
	command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done
python3 -c 'import elftools' 2>/dev/null || { echo "missing: python3-pyelftools" >&2; exit 1; }
[ -f /usr/include/zlib.h ] || { echo "missing: zlib1g-dev (zlib.h)" >&2; exit 1; }
[ -f "$ROOTFS/lib/libc-2.22.so" ] || { echo "$ROOTFS: no lib/libc-2.22.so, not the stock rootfs" >&2; exit 1; }

# Sources, pinned to the commit each tag pointed at. thttpd has no repository
# of its own; the 2.29 tarball's sources are in a pull request of a mirror.
SOURCES=(
	"openssl|https://github.com/openssl/openssl|refs/tags/OpenSSL_1_1_1w|e04bd3433fd84e1861bf258ea37928d9845e6a86"
	"pupnp|https://github.com/pupnp/pupnp|refs/tags/release-1.6.25|ce7d3da274b9bed89d01df20f1e5568327c10f3b"
	"libsndfile|https://github.com/libsndfile/libsndfile|refs/tags/1.2.2|72f6af15e8f85157bd622ed45b979025828b7001"
	"fdk-aac|https://github.com/mstorsjo/fdk-aac|refs/tags/v2.0.3|716f4394641d53f0d79c9ddac3fa93b03a49f278"
	"ntfs-3g|https://github.com/tuxera/ntfs-3g|refs/tags/2022.10.3|78414d93613532fd82f3a82aba5d4a1c32898781"
	"libfuse|https://github.com/libfuse/libfuse|refs/tags/fuse-2.9.9|d04687923194d906fe5ad82dcd546c9807bf15b6"
	"thttpd|https://github.com/larryhe/tinyhttpd|refs/pull/2/head|d02143a240e76dce10d1d01d49c422fdae611a6a"
)

step() { echo; echo "=== $*"; }

# ---------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------
mkdir -p "$WORK/src"
for s in "${SOURCES[@]}"; do
	IFS='|' read -r name url ref commit <<< "$s"
	dir="$WORK/src/$name"
	if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
		step "fetching $name"
		rm -rf "$dir"
		git init -q "$dir"
		git -C "$dir" fetch -q --depth 1 "$url" "$ref"
		git -C "$dir" checkout -q FETCH_HEAD
	fi
	[ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || { echo "$name: not at $commit" >&2; exit 1; }
done

# ---------------------------------------------------------------------------
# the sysroot: the cross glibc's headers, the device's libraries
# ---------------------------------------------------------------------------
step "sysroot"
SYSROOT="$WORK/sysroot"
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT/lib" "$SYSROOT/usr/lib/pkgconfig" "$SYSROOT/usr/include"
cp -a /usr/$CROSS/include/. "$SYSROOT/usr/include/"
cp /usr/include/zlib.h /usr/include/zconf.h "$SYSROOT/usr/include/"
cp "$WORK/src/libfuse/include/"fuse*.h "$SYSROOT/usr/include/"

for lib in ld.so.1 libc.so.6 libpthread.so.0 libdl.so.2 libm.so.6 librt.so.1 libcrypt.so.1 libatomic.so.1 \
	libgcc_s.so.1 libresolv.so.2 libutil.so.1; do
	cp -L "$ROOTFS/lib/$lib" "$SYSROOT/lib/$lib"
done
for link in pthread:libpthread.so.0 dl:libdl.so.2 m:libm.so.6 rt:librt.so.1 crypt:libcrypt.so.1 \
	atomic:libatomic.so.1 resolv:libresolv.so.2 util:libutil.so.1; do
	ln -s "${link#*:}" "$SYSROOT/lib/lib${link%%:*}.so"
done
cp -L "$ROOTFS/usr/lib/libfuse.so.2" "$SYSROOT/usr/lib/libfuse.so.2"
ln -s libfuse.so.2 "$SYSROOT/usr/lib/libfuse.so"

# The C runtime objects and the static part of libc come from the cross glibc;
# the shared libc is the device's.
cp /usr/$CROSS/lib/crt1.o /usr/$CROSS/lib/crti.o /usr/$CROSS/lib/crtn.o /usr/$CROSS/lib/libc_nonshared.a \
	"$SYSROOT/usr/lib/"
cat > "$SYSROOT/usr/lib/libc.so" <<'EOF'
OUTPUT_FORMAT(elf32-tradlittlemips)
GROUP ( /lib/libc.so.6 /usr/lib/libc_nonshared.a AS_NEEDED ( /lib/ld.so.1 ) )
EOF
cat > "$SYSROOT/usr/lib/pkgconfig/fuse.pc" <<EOF
prefix=/usr
libdir=$SYSROOT/usr/lib
includedir=$SYSROOT/usr/include
Name: fuse
Description: the device's libfuse 2.9.9
Version: 2.9.9
Libs: -L\${libdir} -lfuse -pthread
Cflags: -I\${includedir} -D_FILE_OFFSET_BITS=64
EOF

# The compiler, with the sysroot's libraries ahead of the cross toolchain's own
# and the device's ABI: MIPS32r2, o32, 32-bit FPU registers, legacy NaN.
CC="$WORK/cc"
cat > "$CC" <<EOF
#!/bin/sh
exec $GCC --sysroot=$SYSROOT -B$SYSROOT/usr/lib/ -L$SYSROOT/lib -L$SYSROOT/usr/lib \\
	-Wl,-rpath-link,$SYSROOT/lib:$SYSROOT/usr/lib -Wl,-z,max-page-size=0x1000 \\
	-EL -march=mips32r2 -mabi=32 -mfp32 -mnan=legacy -fno-stack-protector -U_FORTIFY_SOURCE "\$@"
EOF
chmod 755 "$CC"

step "compat"
COMPAT="$WORK/compat"
rm -rf "$COMPAT"
mkdir -p "$COMPAT"
for f in $(sed -n 's/^#ifdef F_//p' "$HERE/compat.c"); do
	"$CC" -Os -fPIC -Wall -Werror -DF_$f -c "$HERE/compat.c" -o "$COMPAT/$f.o"
done
$CROSS-ar rcs "$COMPAT/libcompat.a" "$COMPAT"/*.o
"$CC" -Os -fPIC -Wall -Werror -c "$HERE/start.c" -o "$COMPAT/start.o"
LIBCOMPAT="$COMPAT/libcompat.a"
EXE_LDFLAGS="-Wl,--wrap=__libc_start_main $COMPAT/start.o"

# ntfs-3g's configure.ac calls AM_PATH_LIBGCRYPT, which only exists with the
# libgcrypt development files installed. Crypto is off, so a stub does.
mkdir -p "$WORK/m4"
echo 'AC_DEFUN([AM_PATH_LIBGCRYPT],[$3])' > "$WORK/m4/libgcrypt-stub.m4"
export ACLOCAL_PATH="$WORK/m4${ACLOCAL_PATH:+:$ACLOCAL_PATH}"

fresh() { rm -rf "$WORK/build/$1"; mkdir -p "$WORK/build"; cp -a "$WORK/src/$1" "$WORK/build/$1"; cd "$WORK/build/$1"; }
AUTOTOOLS=(--host=$CROSS --prefix=/usr)

mkdir -p "$OUT/usr/lib" "$OUT/usr/bin"

# ---------------------------------------------------------------------------
# OpenSSL: the stock one's platform, directories and options
# ---------------------------------------------------------------------------
step "OpenSSL 1.1.1w"
fresh openssl
CC="$CC" AR=$CROSS-ar RANLIB=$CROSS-ranlib LDLIBS="$LIBCOMPAT" ./Configure linux-generic32 \
	--prefix=/usr --openssldir=/etc/ssl --libdir=lib shared zlib-dynamic threads \
	no-rc5 enable-camellia enable-mdc2 no-tests no-fuzz-libfuzzer no-fuzz-afl \
	-Os -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64
make -j"$JOBS" build_libs
$CROSS-strip --strip-unneeded -o "$OUT/usr/lib/libcrypto.so.1.1" libcrypto.so.1.1
$CROSS-strip --strip-unneeded -o "$OUT/usr/lib/libssl.so.1.1" libssl.so.1.1

# ---------------------------------------------------------------------------
# fdk-aac
# ---------------------------------------------------------------------------
step "fdk-aac 2.0.3"
fresh fdk-aac
autoreconf -fi
CC="$CC" CXX="$CC" CFLAGS=-Os CXXFLAGS=-Os LIBS="$LIBCOMPAT" ./configure "${AUTOTOOLS[@]}" --disable-static
make -j"$JOBS"
$CROSS-strip --strip-unneeded -o "$OUT/usr/lib/libfdk-aac.so.2" .libs/libfdk-aac.so.2

# ---------------------------------------------------------------------------
# libsndfile: no external codec libraries, as the stock one
# ---------------------------------------------------------------------------
step "libsndfile 1.2.2"
fresh libsndfile
autoreconf -fi
CC="$CC" CXX="$CC" CFLAGS=-Os LIBS="$LIBCOMPAT" ./configure "${AUTOTOOLS[@]}" --disable-static \
	--disable-external-libs --disable-mpeg --disable-full-suite --disable-sqlite --disable-alsa --disable-octave
make -j"$JOBS" src/libsndfile.la
$CROSS-strip --strip-unneeded -o "$OUT/usr/lib/libsndfile.so.1" src/.libs/libsndfile.so.1

# ---------------------------------------------------------------------------
# pupnp: client, device, web server, tools and IPv6, with large files, as the
# stock one. dmrd is built against it.
# ---------------------------------------------------------------------------
step "pupnp 1.6.25"
fresh pupnp
./bootstrap
CC="$CC" CFLAGS=-Os LIBS="$LIBCOMPAT" ./configure "${AUTOTOOLS[@]}" --disable-static --enable-ipv6 --disable-samples
make -j"$JOBS"
grep -q '^#define _FILE_OFFSET_BITS 64' upnp/inc/upnpconfig.h || { echo "pupnp: built without large files" >&2; exit 1; }
$CROSS-strip --strip-unneeded -o "$OUT/usr/lib/libupnp.so.6" upnp/.libs/libupnp.so.6
$CROSS-strip --strip-unneeded -o "$OUT/usr/lib/libixml.so.2" ixml/.libs/libixml.so.2
$CROSS-strip --strip-unneeded -o "$OUT/usr/lib/libthreadutil.so.6" threadutil/.libs/libthreadutil.so.6

# ---------------------------------------------------------------------------
# ntfs-3g: the device's libfuse, libntfs-3g linked into the program
# ---------------------------------------------------------------------------
step "ntfs-3g 2022.10.3"
fresh ntfs-3g
autoreconf -fi
PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR= \
	CC="$CC" CFLAGS=-Os LIBS="$LIBCOMPAT" LDFLAGS="$EXE_LDFLAGS" ./configure "${AUTOTOOLS[@]}" --exec-prefix=/usr \
	--with-fuse=external --disable-library --disable-ntfsprogs --disable-ldconfig --disable-plugins \
	--disable-crypto --without-uuid --without-hd --disable-mount-helper
make -j"$JOBS"
$CROSS-strip -o "$OUT/usr/bin/ntfs-3g" src/ntfs-3g

# ---------------------------------------------------------------------------
# thttpd: the default config.h, large files and mmap, as the stock one
# ---------------------------------------------------------------------------
step "thttpd 2.29"
fresh thttpd
ac_cv_func_mmap_fixed_mapped=yes CC="$CC" LIBS="$LIBCOMPAT" LDFLAGS="$EXE_LDFLAGS" ./configure "${AUTOTOOLS[@]}"
make thttpd CCOPT="-Os -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -include $HERE/sigset.h"
$CROSS-strip -o "$OUT/usr/bin/thttpd" thttpd

# ---------------------------------------------------------------------------
step "checking against $ROOTFS"
chmod 755 "$OUT"/usr/lib/* "$OUT"/usr/bin/*
python3 "$HERE/abicheck.py" "$ROOTFS" "$OUT/usr/lib" "$OUT"/usr/lib/* "$OUT"/usr/bin/* \
	"$ROOTFS/usr/bin/dmrd" "$ROOTFS/usr/sbin/wpa_supplicant" "$ROOTFS/usr/bin/adbd"
echo
echo "BUILD_OK -> $OUT"
