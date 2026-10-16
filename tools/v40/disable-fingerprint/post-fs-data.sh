#!/system/bin/sh
# Runs before system_server reads the feature list, so Android never sees a
# fingerprint sensor and never asks the HAL for a session. A bind mount, not a
# file in system/ or vendor/: the metamodule cannot mount /vendor on this
# kernel, and a bind is gone the first boot without this module.
MODDIR=${0%/*}
F=/vendor/etc/permissions/android.hardware.fingerprint.xml
[ -f "$F" ] || exit 0
chcon u:object_r:vendor_configs_file:s0 "$MODDIR/no-fingerprint.xml"
mount -o bind "$MODDIR/no-fingerprint.xml" "$F"
