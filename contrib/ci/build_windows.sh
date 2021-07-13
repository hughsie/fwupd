#!/bin/sh
set -e
#prep
export LC_ALL=C.UTF-8
export DESTDIR=`pwd`/dist
root=`pwd`
build=$root/build-win32
rm -rf $DESTDIR $build

#build
mkdir -p $build $DESTDIR && cd $build
meson .. \
    --cross-file=../contrib/mingw64.cross \
    --prefix=/ \
    --libexecdir=$target \
    --bindir=$target \
    -Dbuild=standalone \
    -Ddocs=none \
    -Dpolkit=false \
    -Dplugin_flashrom=false \
    -Dplugin_uefi_capsule=false \
    -Dplugin_redfish=false \
    -Dplugin_altos=false \
    -Dplugin_dell=false \
    -Dplugin_nvme=false \
    -Dplugin_parade_lspcon=false \
    -Dplugin_realtek_mst=false \
    -Dplugin_platform_integrity=false \
    -Dplugin_tpm=false \
    -Dsystemd=false \
    -Dplugin_emmc=false \
    -Dplugin_amt=false \
    -Dintrospection=false \
    -Dplugin_thunderbolt=false \
    -Dplugin_synaptics_mst=false \
    -Dplugin_synaptics_rmi=false \
    -Dman=false \
    -Dsoup_session_compat=false \
    -Dgcab:introspection=false \
    -Dgcab:docs=false \
    -Dgcab:nls=false \
    -Dgcab:vapi=false \
    -Dgcab:tests=false \
    -Dlibxmlb:introspection=false \
    -Dlibxmlb:gtkdoc=false \
    -Dlibjcat:man=false \
    -Dlibjcat:gpg=false \
    -Dlibjcat:introspection=false \
    -Dgusb:tests=false \
    -Dgusb:docs=false \
    -Dgusb:introspection=false \
    -Dgusb:vapi=false \
    -Dbluez=false \
    -Dgudev=false $@
VERSION=$(meson introspect . --projectinfo | jq -r .version)
ninja -v
ninja -v install

#generate news release
cd $root
contrib/ci/generate_news.py $VERSION > $DESTDIR/news.txt
echo $VERSION > $DESTDIR/VERSION

# create a setup binary
cd $DESTDIR
mkdir -p $DESTDIR/setup
makensis -NOCD $build/contrib/setup-win32.nsi

#so that it's actually executable
cp /usr/x86_64-w64-mingw32/sys-root/mingw/bin/*.dll .

#remove static archives
find -type f -name "*.dll.a" | xargs rm -f

#remove stuff that we really don't need
rm -fr gcab.exe \
       xb-tool.exe \
       share/man \
       include \
       fwupd \
       lib/*.a \
       lib/pkgconfig/ \
       var

export WINEPATH="/usr/x86_64-w64-mingw32/sys-root/mingw/bin/;$build/libfwupd/;$build/subprojects/libxmlb/src/;$build/subprojects/gcab/libgcab/"
#TODO: fixup tests
ninja -C $build test || true
