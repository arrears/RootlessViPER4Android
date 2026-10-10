# Damon DSP — brief progress

Running status of the Damon DSP brief (the Claude Code hand-off doc). One
feature per session; the order of work is the brief's: project rename, then
Features 1 to 3, then Feature 16 (chain builder), then the rest in numbered
order. Before any new stage is placed in the chain, the proposed placement is
printed and waits for Damon's confirmation.

Standing rules for every feature: each stage is its own toggle and off means
true pass-through; nothing resamples or forces 48 kHz; every coefficient comes
from the runtime sample rate; the GitHub Actions run produces the rootless
APK, the rootful APK and the Magisk module every time; new stages are
separate effects next to the stock ones, never edits of ViPER Bass, Dynamic
EQ, Spectrum Extension or Playback Gain Control.

| Step | Status | Notes |
|---|---|---|
| Project rename to Damon DSP | Done (v4.1.0) | See below for exactly what changed and what was deliberately kept |
| Feature 1: Sub-harmonic generator | Not started | Needs chain-placement confirmation first |
| Feature 2: Adaptive bass controller | Not started | |
| Feature 3: Dolby-alike mode | Not started | Research public Dolby Atmos / DAX modules and open Dolby-alike DSP projects; no old V40 module zip |
| Feature 16: Chain builder | Not started | Replaces the fixed order with a chain description read at init |
| Features 4 to 15 | Not started | In numbered order after 16 |

## Rename: what changed

- App label: `Damon DSP` (rootless) and `Damon DSP (Root)` (rootful).
- Application ids: `com.arrears.damondsp.rootless` and `com.arrears.damondsp.rootful`
  (debug builds add `.debug`). The launch allowlist, the Firebase client list
  and the gradle allowlist check carry the new ids; the old
  `com.alienware377.viper4android.*` ids stay in the allowlist so an old
  install still opens.
- Magisk module: id `damondsp_engine`, name `Damon DSP engine`, author
  `arrears`, zip `build/damondsp-engine-module.zip`. Installing it flags the
  old `rv4a_engine` module for removal on the next reboot, since both would
  mount the same engine. AIDL experiment module: `damondsp_effect_aidl`.
- Engine descriptors: the system effect now reports `Damon DSP engine v4` /
  `arrears`, and the app accepts either that or the old
  `RootlessViPER4Android` descriptor as its own engine (same parameter ids).
- Workflows: `Build Damon DSP` (APKs + module, artifacts `DamonDSP-rootless-apk`,
  `DamonDSP-rootful-apk`, `DamonDSP-magisk-module`) and `Build Magisk module`
  (artifact `damondsp-engine-module`) now also run on `damon-dsp` and
  `ccr-*` branches.
- Output file names: `DamonDSP-v<version>-<variant>-...apk`; backups are
  `damondsp_backup_<date>.tar.gz` (old `jamesdsp_backup_` files still count
  towards the backup limit and still restore).
- About screen: project page points at `arrears/RootlessViPER4Android`, the
  upstream Google Play entry is gone, credits list Damon DSP, the
  RootlessViPER4Android fork and the original authors. Version line reads
  `Damon DSP x.y.z — forked from RootlessViPER4Android, based on RootlessJamesDSP n`.
- User-facing strings, README, privacy policy and Magisk README say Damon DSP.

## Rename: deliberately kept

- Kotlin namespace `me.timschneeberger.rootlessjamesdsp` and the per-effect
  preference files (`dsp_*`): internal only, and renaming them would break
  preset and backup compatibility for no user-visible gain.
- C identifiers (`rv4a_*`) and the AIDL service binary names: internal.
- The Rubber Band add-on still downloads from the RootlessViPER4Android
  release that built it.
- Translations and fastlane store metadata still carry upstream names; the
  fork never published them.
- The GitHub repository itself is still named `RootlessViPER4Android`; a
  rename on GitHub redirects the old links, so nothing here needs to change
  when Damon renames it.
- No status strip exists yet; it arrives with Feature 4 and will be named
  for Damon DSP from the start.
