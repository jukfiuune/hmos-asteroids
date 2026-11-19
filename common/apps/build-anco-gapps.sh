#!/bin/bash
# Google Play for the Android container (build PC): Google Play services, the Services Framework and the Play Store
# from MindTheGapps (Android 16, arm64), as trees the boot step binds over the guest's (prep.d/65-anco.sh, when
# /data/asteroids/anco/gapps exists; staged by stage-anco.sh). The rest of the package (setup wizard, keyboard,
# assistant, ...) stays out.
# The guest's /product and /system_ext are /system/product and /system/system_ext in its read-only image, and a bind
# needs an existing target, so each tree replaces a whole guest directory: the empty priv-app directories take the
# apps, and the few configuration directories are merged copies (the guest's files plus Google's privileged
# permissions, sysconfig, default permissions and GmsOverlay).
#   build-anco-gapps.sh [MindTheGapps zip]
# Output: $HARMONY_BUILD/fw/anco-gapps/{gapps/, anco-gapps.tar}. stage-anco.sh labels them on the phone: each copied
# guest file as the guest image has it (SELinux contexts, passwd, aconfig storage, ...), new ones as aoco_system_file.
set -e
HB=${HARMONY_BUILD:-$HOME/harmony-build}
B=$HB
ZIP=${1:-$B/fw/gapps/dl/MindTheGapps-16.0.0-arm64-20261023_034858.zip}
G=$B/fw/bre-al80/anco_x/system
O=$B/fw/anco-gapps
X=$O/zip
rm -rf $O && mkdir -p $X
unzip -q "$ZIP" -d $X
M=$X/system
W=$O/gapps
# The apps (into the guest's empty priv-app directories).
mkdir -p $W/product/priv-app $W/system_ext/priv-app
cp -a $M/product/priv-app/GmsCore $M/product/priv-app/Phonesky $W/product/priv-app/
cp -a $M/system_ext/priv-app/GoogleServicesFramework $W/system_ext/priv-app/
# Merged configuration directories: the guest's, then Google's.
cp -a $G/product/etc/permissions $G/product/etc/sysconfig $G/product/overlay -t $W/product/ 2>/dev/null || true
mkdir -p $W/product/etc && mv $W/product/permissions $W/product/sysconfig $W/product/etc/
cp -a $M/product/etc/permissions/privapp-permissions-google-product.xml $M/product/etc/permissions/privapp-permissions-mtg.xml \
    $W/product/etc/permissions/
cp -a $M/product/etc/sysconfig/google.xml $M/product/etc/sysconfig/google-hiddenapi-package-allowlist.xml \
    $M/product/etc/sysconfig/sysconfig-mtg.xml $W/product/etc/sysconfig/
cp -a $M/product/overlay/GmsOverlay.apk $W/product/overlay/
# system_ext/etc whole (the guest has no default-permissions directory anywhere, and a bind needs one to exist).
cp -a $G/system_ext/etc $W/system_ext/
cp -a $M/system_ext/etc/permissions/privapp-permissions-google-system-ext.xml $W/system_ext/etc/permissions/
mkdir -p $W/system_ext/etc/default-permissions
cp -a $M/product/etc/default-permissions/*.xml $W/system_ext/etc/default-permissions/
# The guest's build.prop for /product with ro.pms.hide_anco_icon off: Huawei's package converter (hwServices.jar,
# ancoproxy Converter.isHideDesktopIcon) hides the Play Store's and Play services' icons from the HarmonyOS home
# screen while that property is true (its default).
cp -a $G/product/etc/build.prop $W/product/etc/build.prop
echo ro.pms.hide_anco_icon=false >> $W/product/etc/build.prop
rm -rf $X
(cd $O && tar -cf anco-gapps.tar gapps)
echo "=== $O/anco-gapps.tar ($(du -sh $W | cut -f1)) ==="
