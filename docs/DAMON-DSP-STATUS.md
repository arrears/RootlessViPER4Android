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
| Feature 1: Sub-harmonic generator | Done (v4.1.0) | Placement confirmed: inside the bass block, with the other bass processors. See below |
| Stable signing keystore and GitHub Releases | Done (v4.1.1) | Release-key signing enforced in CI, module bundles the rootful app, `v*` tags self-publish. See below |
| Feature 2: Adaptive bass controller | Not started | Runs inside the same bass block; drives a low shelf on the low band and Feature 1's Mix |
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

## Feature 1: Sub-harmonic generator

Card "Sub-harmonic generator" (preference namespace `dsp_subharmonic`, chain
id 39, HAL parameter ids 26022 / 26122). Controls: Mix (-40 to 0 dB, default
-12; the bottom of the slider is off), Sweep (band centre, 30 to 160 Hz,
default 77), Wide (band width in octaves, 0.5 to 3, default 1.25, so the
default band is about 50 to 120 Hz), Dynamic (default on).

What was built:

- `Effects/bassblock.c`: the shared split every bass stage runs inside. One
  Kaiser-windowed linear-phase FIR low-pass at 120 Hz (-6 dB there, 60 dB
  down by 160 Hz), FFT-partitioned through the engine's own convolver. The
  dry signal is delayed by the filter's group delay, so at neutral the block
  is a bit-exact delay. Stages return what they ADD to the low band; the dry
  signal is never altered. The addition goes through a lookahead gain dip
  (4 ms lookahead, sliding-window minimum, 150 ms release) against a ceiling
  2% under the master limiter's threshold, so bass alone never reaches the
  limiter. Allocated when the first stage is enabled, freed when the last is
  disabled, redesigned on every sample-rate change. Latency 26.6 ms at every
  rate; `BassBlockLatency()` reports it for the later video sync mode.
- `Effects/subharmonic.c`: detector band-pass (4th-order Butterworth edges,
  double precision) on the mono low band; rising zero crossings timed to a
  fraction of a sample give the period, the peak of each cycle gives the
  amplitude; an oscillator at half the frequency is phase-locked by nudging
  its frequency over the next cycle (no phase steps, no three-halves
  product); output low-passed at 80 Hz, 8th order. Dynamic mode follows the
  cycle peaks; static mode follows an 80 ms / 600 ms average of them. A note
  outside the band (by period) gets no sub at all.
- Both engine paths: JNI `setSubHarmonic` (rootless) and HAL ids (rooted).
- Chain order dialog now also lists Vinyl and Balance, which were missing.

Verified by `tools/subharmonic_harness.c` at 44.1, 48 and 192 kHz with 192,
1024 and 4096-sample blocks: neutral is bit-exact, a 100 Hz note grows a
50 Hz sub at about -14 dB with nothing at 150, 200 or 300 Hz, the null
against bypass at 240 Hz and above at full mix is within 0.01 dB (brief asks
0.5 dB), a hot 60 Hz note with a full sub peaks at 0.98, and the delay in
milliseconds is the same at 48 and 192 kHz. `tools/chain_harness.c` proves
the new stage is dispatched even from a saved chain order that predates it.

Left for listening on the Redmi Note 10 Pro: defaults (Mix, band, the 80 Hz
low-pass) are the brief's and will want tuning by ear. Not yet done:
exposing the current Mix and the bass-band meters for the Feature 5
visualiser, and the minimum-phase swap for Feature 12.


## Signing keystore and releases

Done in v4.1.1, as the "Signing keystore and release" section of the brief
asks, with the order the hand-off doc insists on: keystore first, then the
build Damon keeps.

- One release keystore (RSA 2048, PKCS12, alias `damondsp`, valid 30 years),
  generated once with keytool and never committed. It lives in four
  repository secrets: `SIGNING_KEYSTORE` (the file, base64), `KEY_ALIAS`,
  `KEYSTORE_PASSWORD`, `KEY_PASSWORD`.
- `app/build.gradle.kts` signs release builds with it when
  `DAMONDSP_KEYSTORE_FILE` and friends are in the environment, and with
  `DAMONDSP_REQUIRE_RELEASE_KEY=1` (set by CI) refuses to build at all
  without them. A local build without the keystore still works, debug-signed,
  with a warning: that build will not update over a release-signed install.
- The `Build Damon DSP` workflow unpacks the keystore first and fails before
  building if any secret is missing; builds the rootless and rootful release
  APKs; verifies every APK with `apksigner` and compares its signer to the
  keystore's certificate; prints the certificate SHA-256 in the log and the
  run summary; builds the Magisk module with the signed arm64 rootful APK
  inside it (`customize.sh` installs the app when flashed from the Magisk app;
  from recovery it says to install the APK by hand); runs
  `ci/check-module.sh`; and on a `v*` tag, or a manual run with
  `release_tag`, publishes a GitHub Release with the module zip, the rootless
  arm64 APK and the rootful arm64 APK attached. The body comes from
  `docs/releases/<tag>.md` plus the certificate and an install reminder
  (`ci/release-notes.sh`).
- `ci/check-module.sh` holds every module check (module.prop in step with
  the app, LF line endings, scripts parse under dash, nothing written outside
  the module directory, 16 KB page alignment, no TEXTREL, no executable
  stack, soname, the four exported symbols, zip paths, bundled APK); both
  workflows call it, so they cannot drift.
- Upstream's `build.yml` no longer fires on tags; it would have run a second,
  failing build beside each release.
- One-time cost, unchanged from the brief: moving from the alienware377 4.0.0
  build, or from the debug-signed 4.1.0 Feature 1 build, to the first
  release-signed build needs one uninstall and one ADB capture grant. After
  that the key never changes.
