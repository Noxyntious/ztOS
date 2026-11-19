#!/bin/sh
# mkglext.sh: flatpak GL extension "husky" = Mesa 26.3 (Panfrost, Mali-G715 / v11) for the 26.08 runtime
set -e
E=/var/lib/flatpak/extension/org.freedesktop.Platform.GL.husky/aarch64/26.08
rm -rf $E; mkdir -p $E/lib/gbm $E/glvnd/egl_vendor.d $E/share
cd $E && tar -xf /tmp/mesa263-ext.tar
cd $E/lib
ln -s libEGL_mesa.so.0.0.0 libEGL_mesa.so.0
ln -s libgbm.so.1.0.0 libgbm.so.1
cp -a /usr/lib/libSPIRV-Tools.so .
cp -a /usr/lib/libsensors.so.5.0.0 . && ln -s libsensors.so.5.0.0 libsensors.so.5
printf '{\n    "file_format_version" : "1.0.0",\n    "ICD" : {\n        "library_path" : "libEGL_mesa.so.0"\n    }\n}\n' > $E/glvnd/egl_vendor.d/50_mesa.json
chown -R root:root /var/lib/flatpak/extension/org.freedesktop.Platform.GL.husky
find /var/lib/flatpak/extension/org.freedesktop.Platform.GL.husky | sort
