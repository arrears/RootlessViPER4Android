#!/system/bin/sh
# Nothing asks for the HAL once the feature is hidden; stopping it as well
# means it cannot crash on its own either.
stop vendor.fingerprint-default
