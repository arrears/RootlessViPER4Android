#!/usr/bin/env bash
# Everything the Magisk module is checked for before it may be shipped, in one
# place, so the release workflow and the module-only workflow cannot drift
# apart on what "good" means. Run from the repository root after
# magisk/build-module.sh.
#
# What is checked and why each check exists is explained inline; every one of
# them was added after a real failure that nothing else would have caught.
set -eu

ZIP="${1:-build/damondsp-engine-module.zip}"
LIBS="${2:-build/magisk/libs}"
STAGED="$(dirname "$LIBS")"
fail=0

# --- the committed module.prop must say what the app says --------------------
# build-module.sh stamps the packaged copy regardless, but a committed file
# that disagrees means the version bump was half done.
APP_NAME=$(sed -n 's/.*versionName *= *"\([^"]*\)".*/\1/p' buildSrc/src/main/kotlin/AndroidConfig.kt | head -1)
APP_CODE=$(sed -n 's/.*versionCode *= *\([0-9][0-9]*\).*/\1/p' buildSrc/src/main/kotlin/AndroidConfig.kt | head -1)
echo "app is v$APP_NAME ($APP_CODE)"
grep -q "^version=v$APP_NAME$" magisk/module.prop || { echo "magisk/module.prop version is not v$APP_NAME"; fail=1; }
grep -q "^versionCode=$APP_CODE$" magisk/module.prop || { echo "magisk/module.prop versionCode is not $APP_CODE"; fail=1; }
grep -q "^version=v$APP_NAME$" "$STAGED/module.prop" || { echo "packaged module.prop version is not v$APP_NAME"; fail=1; }
grep -q "^versionCode=$APP_CODE$" "$STAGED/module.prop" || { echo "packaged module.prop versionCode is not $APP_CODE"; fail=1; }

# --- the scripts ---------------------------------------------------------------
# A carriage return in post-fs-data.sh makes the boot watchdog create a file
# Magisk does not recognise, which leaves the module active with its only
# safety net dead.
for f in magisk/*.sh magisk/common/*.sh magisk/module.prop; do
  if grep -q $'\r' "$f"; then echo "carriage returns in $f"; fail=1; fi
done
for f in magisk/*.sh magisk/common/*.sh; do
  sh -n "$f" || { echo "$f is not valid shell"; fail=1; }
  # dash is closer to the device's mksh than bash is
  if command -v dash >/dev/null 2>&1; then
    dash -n "$f" || { echo "$f is not valid POSIX shell"; fail=1; }
  fi
done
# Every path the module writes must stay inside $MODPATH. Escaping it is what
# makes a module able to leave damage behind after removal.
if grep -nE '(^|[^A-Za-z_])(rm|mv|cp|ln|touch|mkdir)\b[^|]*(/data/adb/(service\.d|post-fs-data\.d)|/system/(bin|lib|etc)/)' magisk/*.sh magisk/common/*.sh | grep -v MODPATH | grep -v '^\s*#'; then
  echo "a script writes outside the module directory"
  fail=1
fi

# --- every built library -----------------------------------------------------
# The .so is what the audio server dlopens. Three properties make the
# difference between loading and a silent skip, and none of them are visible
# without looking. The NDK's llvm tools are preferred; binutils print the same
# columns for what is grepped here.
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_ROOT:-}}}"
READELF=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-readelf 2>/dev/null | head -1 || true)
NM=$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-nm 2>/dev/null | head -1 || true)
[ -n "$READELF" ] || READELF=$(command -v llvm-readelf || command -v readelf || true)
[ -n "$NM" ] || NM=$(command -v llvm-nm || command -v nm || true)
[ -n "$READELF" ] && [ -n "$NM" ] || { echo "no readelf/nm available to inspect the libraries"; exit 1; }
n=0
for so in "$LIBS"/*/libjamesdsp.so; do
  [ -f "$so" ] || continue
  n=$((n + 1))
  echo "--- $so"
  # 16 KB pages: Android 15 devices refuse a 4 KB-aligned library.
  if "$READELF" -l "$so" | grep -q "LOAD.*0x1000$"; then
    echo "  4 KB aligned, would not load on a 16 KB page device"; fail=1
  fi
  # Text relocations are refused since Android 10.
  "$READELF" -d "$so" | grep -q TEXTREL && { echo "  has TEXTREL"; fail=1; }
  # An executable stack is refused since Android 14.
  "$READELF" -l "$so" | grep -E "^\s+GNU_STACK" | grep -q "RWE" && { echo "  executable stack"; fail=1; }
  # The soname is what bionic registers it under.
  "$READELF" -d "$so" | grep -qi "soname.*libjamesdsp.so" || { echo "  wrong soname"; fail=1; }
  # Without these four the library loads and the effect never appears.
  for sym in AELI EffectCreate EffectRelease EffectGetDescriptor; do
    "$NM" -D --defined-only "$so" | grep -qw "$sym" || { echo "  missing $sym"; fail=1; }
  done
  echo "  $(stat -c%s "$so") bytes"
done
[ -f "$LIBS/arm64-v8a/libjamesdsp.so" ] || { echo "no arm64-v8a engine was built"; fail=1; }
[ "$n" -ge 1 ] || { echo "no libraries found under $LIBS"; fail=1; }

# --- the packaged zip --------------------------------------------------------
[ -f "$ZIP" ] || { echo "$ZIP does not exist"; exit 1; }
unzip -l "$ZIP"
# A zip written by a Windows tool stores literal backslashes, and the module
# then installs as files with backslashes in their names instead of
# directories, doing nothing at all. Checked against the raw listing, because
# Python normalises separators on read.
if unzip -l "$ZIP" | grep -qU '\\\\'; then
  echo "backslash paths in the archive"; fail=1
fi
for f in module.prop customize.sh post-fs-data.sh service.sh common/patch_audio_config.sh libs/arm64-v8a/libjamesdsp.so; do
  unzip -l "$ZIP" | grep -q " $f$" || { echo "missing $f"; fail=1; }
done
# The rooted app is bundled on release builds; a module without it is not
# what the release page promises.
if [ "${DAMONDSP_EXPECT_APK:-0}" = "1" ]; then
  unzip -l "$ZIP" | grep -q " DamonDSP-rootful.apk$" || { echo "missing the bundled DamonDSP-rootful.apk"; fail=1; }
fi

if [ "$fail" -ne 0 ]; then echo "MODULE CHECKS FAILED"; exit 1; fi
echo "module checks passed: v$APP_NAME ($APP_CODE), $n libraries, $(stat -c%s "$ZIP") byte zip"
