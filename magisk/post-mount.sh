#!/system/bin/sh
# Mounts this module's /vendor (and /odm) files itself when the root manager's
# own mount left them out.
#
# KernelSU-Next hands module mounting to a metamodule. meta-overlayfs mounts
# /system fine, but on kernels older than 5.2 it cannot re-attach the
# partitions mounted inside /vendor (/vendor/dsp and /vendor/firmware_mnt on
# Qualcomm devices): the call it uses for that does not exist there, so it
# takes the whole /vendor overlay back down and still reports success. The
# engine then sits in /system only, and /vendor/etc/audio_effects.xml stays the
# device's own, with no entry for us in it. Nothing loads, and nothing says why.
#
# KernelSU and APatch run this after their mount and before the audio HAL
# starts. Magisk has no post-mount stage and never runs it; it mounts /vendor
# itself. Where the files are already in place this does nothing.

MODDIR=${0%/*}
# Every mount below is built from this.
[ -n "$MODDIR" ] && [ -d "$MODDIR" ] || exit 0
[ -f "$MODDIR/disable" ] && exit 0

log() { echo "post-mount: $*" >> "$MODDIR/boot.log" 2>/dev/null; }

# The files we ship for a partition, never ones post-fs-data.sh set aside: a
# config it quarantined or rejected must not come back in through here.
payload() {
  find "$MODDIR/$1" -type f ! -name "*.disabled" ! -name "*.rejected" ! -name "*.tmp" 2>/dev/null
}

# Compared by content rather than by asking /proc/mounts, so it holds whatever
# put the file there: a metamodule that works, AML, or this script.
in_place() {
  for f in $(payload "$1"); do
    cmp -s "$f" "/$1${f#"$MODDIR/$1"}" || return 1
  done
  return 0
}

for P in vendor odm; do
  [ -d "$MODDIR/$P" ] || continue
  # A symlinked partition is reached through /system, which is already done.
  [ -L "/$P" ] && continue
  [ -n "$(payload "$P")" ] || continue
  if in_place "$P"; then
    log "/$P already carries our files"
    continue
  fi
  log "/$P was not mounted for us; mounting our files directly"

  # Libraries first. A config naming a library that is not there is the one
  # thing worse than no config, so the configs wait until the engine is in.
  mounted=""
  for f in $(payload "$P" | grep '\.so$'); do
    rel=${f#"$MODDIR/$P"}
    target="/$P$rel"
    # A second library in a directory already overlaid is already in place.
    cmp -s "$f" "$target" && continue
    dir="/$P$(dirname "$rel")"
    if [ -e "$target" ]; then
      mount -o bind "$f" "$target" && mounted="$target $mounted"
    elif [ -d "$dir" ]; then
      # An overlay on the soundfx directory alone. Nothing is mounted inside
      # it, which is exactly what the partition-wide overlay tripped over, and
      # this is the classic mount call that every kernel has.
      mount -t overlay -o "ro,lowerdir=$MODDIR/$P$(dirname "$rel"):$dir" KSU "$dir" &&
        mounted="$dir $mounted"
    fi
  done

  ok=1
  for f in $(payload "$P" | grep '\.so$'); do
    target="/$P${f#"$MODDIR/$P"}"
    if cmp -s "$f" "$target"; then log "in place: $target"; else log "could not place $target"; ok=0; fi
  done
  if [ "$ok" != "1" ]; then
    for m in $mounted; do umount -l "$m" 2>/dev/null; done
    log "engine is not in /$P, so its config was left alone too"
    continue
  fi

  # Configs replace a file the device already has, so binding that one file is
  # enough. One the device does not have is not ours to invent here.
  for f in $(payload "$P" | grep -v '\.so$'); do
    target="/$P${f#"$MODDIR/$P"}"
    cmp -s "$f" "$target" && continue
    if [ ! -f "$target" ]; then
      log "skipped $target: the device has no such file"
      continue
    fi
    if mount -o bind "$f" "$target" && cmp -s "$f" "$target"; then
      log "in place: $target"
    else
      log "could not place $target"
    fi
  done
done

exit 0
