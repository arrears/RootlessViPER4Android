# LG V40 (judypn) helper modules

Two small KernelSU modules for the LG V40 this project is tested on (crDroid,
Android 16, kernel 4.9, KernelSU-Next). They are separate from the Damon DSP
engine module on purpose: both work around this device and this ROM, and
neither belongs in a module meant for any phone.

Both carry `skip_mount` and change nothing on the partitions. Removing the
module in the root manager and rebooting undoes it completely.

## quaddac-switch-fix

Switching the Quad DAC on moves the ES9218 into HiFi mode and hands it the
headphone jack. The ROM's audio HAL is meant to start feeding the DAC at the
same time, but it logs "It is not HiFi DAC scenario" and keeps the stream on
the ordinary headphone path, so the music goes silent until something puts the
chip back in bypass - unplugging the headphones, for instance.

`service.sh` waits on `persist.vendor.lge.audio.hifi_dac` (with
`resetprop -w`, so it sleeps rather than polls). After each switch it watches
the chip for a few seconds; if the chip reaches Hifi while the HAL's latest
routing is plain `headphones`, it sets `Es9018 State` to `Bypass` - the same
transition a replug causes. If the HAL ever does pick `headphones-hifi-dac`,
it leaves the chip alone. Each decision is logged to `switch.log` in the
module directory.

What it does not do: make the HAL actually use the DAC. On this ROM the HAL
did not route to `headphones-hifi-dac` with or without Damon DSP attached, so
"DAC on" currently plays through the normal headphone output. The module only
stops the switch from going silent.

## disable-fingerprint

For a V40 whose fingerprint sensor is disconnected, where the fingerprint HAL
crashes whenever the framework opens a session on it. `post-fs-data.sh`
bind-mounts an empty permissions file over
`/vendor/etc/permissions/android.hardware.fingerprint.xml`, so Android no
longer reports the feature and never asks the HAL for anything; `service.sh`
stops the HAL as well. A bind mount rather than a file under `vendor/`,
because meta-overlayfs cannot mount `/vendor` on this kernel.

## Packaging and installing

The zip must hold the files at its root, with forward slashes:

```
cd tools/v40/quaddac-switch-fix && zip -r ../quaddac-switch-fix.zip . && cd -
adb push tools/v40/quaddac-switch-fix.zip /data/local/tmp/
adb shell su -c 'ksud module install /data/local/tmp/quaddac-switch-fix.zip'
```

then reboot. The same goes for `disable-fingerprint`.
