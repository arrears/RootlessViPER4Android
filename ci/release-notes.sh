#!/usr/bin/env bash
# Assembles the body of a GitHub Release from the hand-written notes for the
# tag (docs/releases/<tag>.md, the features list) plus what only the build
# knows: the signing certificate, the module version and the file names.
#
#   ci/release-notes.sh <tag> <cert-sha256> <module.zip> <rootless.apk> <rootful.apk>
set -eu
TAG="$1"; CERT="$2"; MODULE="$3"; ROOTLESS="$4"; ROOTFUL="$5"
NOTES="docs/releases/$TAG.md"
MODVER=$(sed -n 's/^version=//p' build/magisk/module.prop | head -1)
MODCODE=$(sed -n 's/^versionCode=//p' build/magisk/module.prop | head -1)

if [ -f "$NOTES" ]; then
  cat "$NOTES"
else
  echo "## Damon DSP $TAG"
  echo
  echo "No hand-written notes for this tag (add docs/releases/$TAG.md). Changes since the previous tag are in the commit list below."
fi

cat <<NOTES

## Files

| File | What it is |
|---|---|
| \`$(basename "$MODULE")\` | Magisk module: the shared engine for every ABI, with the signed rootful app inside. Rooted phones (LG V40, Redmi Note 10 Pro). |
| \`$(basename "$ROOTLESS")\` | Rootless standalone app, arm64. Unrooted phones (Samsung Galaxy S24 Ultra). |
| \`$(basename "$ROOTFUL")\` | The rootful app on its own, arm64, for installing by hand if the module was flashed from recovery. |

## Signing

Every APK here is signed with the stable Damon DSP release key. Certificate SHA-256:

\`\`\`
$CERT
\`\`\`

It must read the same on every future release; check any APK with
\`apksigner verify --print-certs\`.

Module: \`$MODVER\` (versionCode $MODCODE).

## Installing

- **Magisk module:** flash the zip from the Magisk app and reboot. Flashed from
  the app it also installs the rootful app; flashed from recovery it does not,
  so install the rootful APK above by hand. (From the Actions tab the module
  arrives double-zipped and has to be unzipped once; this release file flashes
  as it is.)
- **Rootless APK:** on the first install over a build signed with a different
  key (the alienware377 4.0.0 build, or any build before this key existed)
  uninstall the old app first, then grant the capture permission once over ADB
  as the app's first-run screen explains. Every later release installs as an
  in-place update and keeps that grant.
NOTES
