#!/system/bin/sh
# LG V40 Quad DAC switch fix.
#
# Switching the Quad DAC on moves the ES9218 into HiFi mode and hands it the
# headphone jack (hph_switch = 1). The ROM's audio HAL is meant to start
# feeding the DAC at the same time, but it decides "It is not HiFi DAC
# scenario" and keeps the stream on the ordinary headphone path - which the
# jack is no longer connected to - so the music goes silent until something
# puts the chip back in bypass. Unplugging the headphones does that; this
# does it too, through the driver's own control, the same transition.
#
# It only steps in when the HAL's latest routing decision was plain
# "headphones". If the HAL ever does pick the real DAC route
# (headphones-hifi-dac), the chip is left alone.
MODDIR=${0%/*}
LOG="$MODDIR/switch.log"
PROP=persist.vendor.lge.audio.hifi_dac
RP=/data/adb/ksu/bin/resetprop
[ -x "$RP" ] || RP=resetprop

until [ "$(getprop sys.boot_completed)" = "1" ]; do sleep 2; done

last=$(getprop $PROP)
echo "$(date): watching $PROP (now $last)" > "$LOG"

chip_state() {
  tinymix "Es9018 State" 2>/dev/null | sed -n 's/.*>\([A-Za-z]*\).*/\1/p'
}

while true; do
  "$RP" -w --timeout 3600 $PROP "$last" >/dev/null 2>&1 || sleep 2
  now=$(getprop $PROP)
  [ "$now" = "$last" ] && continue
  last=$now

  # The driver reaches HiFi some time after the setting changes - sometimes
  # more than a second - so watch for a few seconds rather than look once.
  # A single look missed a lone switch-on and left it silent.
  acted=0
  i=0
  while [ $i -lt 8 ]; do
    sleep 0.5
    i=$((i + 1))
    [ "$(chip_state)" = "Hifi" ] || continue
    route=$(logcat -d -b main -t 400 2>/dev/null | grep -o 'out_snd_device([a-z-]*)' | tail -1)
    [ "$route" = "out_snd_device(headphones-hifi-dac)" ] && break
    tinymix "Es9018 State" Bypass >/dev/null 2>&1
    echo "$(date): DAC $now; chip went Hifi after ${i}x0.5s but HAL routes ${route:-unknown}; set Bypass -> $(chip_state)" >> "$LOG"
    acted=1
    break
  done
  [ $acted = 1 ] || echo "$(date): DAC $now; chip $(chip_state); left alone" >> "$LOG"
done
