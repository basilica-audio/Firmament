# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] - 2026-07-16

Research-driven deep-dive rework (`docs/design-brief.md`, `docs/research-notes.md`)
plus the suite's M2 preset system (ported from `basilica-audio/nave`'s pilot
implementation) and a German i18n frame for the preset UI. Every new
parameter defaults to a value that reproduces v0.1.1 behaviour exactly, so
existing sessions/automation are unaffected unless a user or preset opts in.

### Added

- **Decorrelate** (`decorrelateEnabled`, `decorrelateAmount`): a second
  alternative widening technique for near-mono material, alongside Haas Mode
  - a cascade of allpass IIR filters processes the Right channel instead of
  delaying it, trading Haas Mode's deep, well-documented comb-filter
  mono-fold-down cost for much smaller, documented "mild spectral ripple."
  Mutually exclusive with Haas Mode (Decorrelate takes effect, Haas Mode's
  delay line is bypassed, whenever both are engaged). Off by default.
  Sourced from iZotope Ozone Imager's dual Stereoize modes and the general
  allpass-decorrelation literature (`docs/research-notes.md` Section 4) - the
  headline finding of this deep-dive's research pass.
- **Auto Mono Safety ballistics/dead-zone/floor/multiband revisions**:
  ballistics moved from a 200 ms to a 300 ms leaky-integrator time constant
  (closer to, though deliberately not all the way to, the ~600 ms documented
  for a passive correlation-meter display); a new dead-zone means correlation
  in `[-0.10, 1.0]` no longer triggers any attenuation at all ("the
  occasional small deviation into the negative side is usually
  insignificant"); the previously-hardcoded 0.35 linear floor gain is now the
  user-adjustable `autoMonoSafetyFloorDb` parameter (-24 to 0 dB, default
  -9.1 dB - reproduces the old value exactly); and a new
  `autoMonoSafetyMultiband` parameter (off by default) lets Auto Mono Safety
  reason about the low/high bands split out by Bass Mono Freq independently,
  instead of one broadband correlation estimate scaling both - sourced from
  the per-band correlation-safety precedent of KERN WIDE, HoRNet ZeroWidth,
  and In The Mix Bandwidth.
- Bass Mono Freq's range ceiling extended from 500 to 600 Hz (skew unchanged)
  - the single lowest-confidence, most-reasoned change in this release,
  based on the Waves S1 Shuffle control's documented ~600 Hz convention (an
  indirect, third-party-summarised source, not a primary manual).
- M2 preset system (`src/presets/`): factory/user preset browsing, save/save
  as/rename/delete, a settable default, single-file and zip-bank
  import/export, and a dirty-state indicator, via a preset bar docked at the
  top of the editor. Ten factory presets ship (`docs/presets.md`) - nine
  sourced from `docs/design-brief.md`'s Factory Presets section plus a
  `Default`/`Init` passthrough preset. Ported verbatim from
  `basilica-audio/nave`'s M2 pilot implementation.
- German frame-string localisation (`resources/i18n/de.txt`), selected
  automatically from the system language at editor construction. Only the
  preset bar's frame strings are translated - parameter names/units are
  never translated anywhere in this plugin.
- `docs/design-brief.md` and `docs/research-notes.md`: the full sourced
  research behind every default/range in this release, including which
  numbers are directly sourced vs. reasoned.
- `docs/presets.md`: one-line intent documentation for every factory preset.
- Broadened Catch2 test suite (51 -> 82 test cases): dead-zone/floor/
  multiband/ballistics regression tests for Auto Mono Safety, Decorrelate
  mono-fold-down cost (measured via a real magnitude-spectrum FFT
  comparison, not just described) and mutual-exclusivity tests, a Bass Mono
  Freq range-extension sweep, a tolerant v0.1.1-state-import test, and 17
  ported preset-system tests plus 3 new i18n-coverage tests.

### Changed

- `PluginEditor`: docked a preset bar at the top of the v0.1/v0.2-style
  functional editor, and added controls for every new v0.2.0 parameter
  (`Auto Mono Safety Floor` knob, `Auto Mono Safety Multiband`/`Decorrelate`
  toggles, `Decorrelate Amount` knob).
- `docs/architecture.md` and `docs/manual.md`: updated for the full v0.2.0
  signal path, parameter reference, and a new honesty note on the
  research-derived (not measured-against-a-commercial-plugin) nature of this
  release's voicing.

### Fixed

- Nothing DSP-behavioural beyond the deliberate v0.2.0 changes documented
  above - v0.1.1's issue #12/#13 fixes (live crossover state while bass-mono
  is disabled; smoothed Auto Mono Safety toggle) are preserved and their
  regression tests still pass unmodified.

## [0.1.1] - 2026-07-16

### Added

- App icon: the plugin/standalone bundles now ship with the Firmament icon (`docs/assets/icon.png`, wired via `juce_add_plugin`'s `ICON_BIG`), per the suite-wide mandate that app icons ship from this version onward.

### Fixed

- Bass-mono crossover: the Linkwitz-Riley crossover's internal filter state was frozen while the section was disabled (`Bass Mono Freq` at 0 Hz), so re-engaging it - e.g. automation sweeping back up through 0 Hz - resumed filtering from a stale snapshot and produced an audible transient. The crossover now keeps tracking the live Side signal while disabled (the same "always process, conditionally use" pattern Haas Mode's delay line already used), making re-engagement transient-free. ([#12](https://github.com/basilica-audio/firmament/issues/12))
- Auto Mono Safety: flipping the on/off toggle applied the correlation-derived Side attenuation as an instant step - up to ~9 dB in a single sample when the (always-running) correlation estimate was already settled at its floor. The toggle now crossfades between bypassed and engaged over the same ~50 ms smoothing window used by the other parameters. ([#13](https://github.com/basilica-audio/firmament/issues/13))
- Release workflow: the tag-triggered release build uploaded assets into a GitHub release that no job had created ("release not found"); an idempotent `create-release` job now creates the release object before both build jobs run (pattern reconciled with `basilica-audio/crypta`).

## [0.1.0] - 2026-07-14

### Added

- Project bootstrap: README, license, contributing guide, architecture and build docs, ADRs, and CI workflow.
- DSP core: initial working Firmament signal path (Width, Bass Mono crossover, Output trim) with unit tests.
- Multiband width: independent `Low Width` (0-200%, default 0%) and `Width` controls for the Side signal's low/high bands, split at `Bass Mono Freq`. `Low Width`'s default exactly reproduces the original "bass mono forces the low band to silence" behaviour.
- Auto Mono Safety: an optional, correlation-driven Side attenuation that reins in the stereo image whenever the input goes strongly out-of-phase, without ever touching Mid (so the mono-sum invariant holds regardless of whether it is engaged).
- Correlation/phase meter (DSP): a running, leaky-integrated input-correlation estimate exposed via `FirmamentEngine::getCorrelationValue()` and `FirmamentAudioProcessor::getCorrelationMeterValue()`, driving Auto Mono Safety internally and ready for a future GUI meter widget (M3).
- Haas Mode: an optional alternative widening technique (0-40 ms Right-channel delay after M/S decode, via `juce::dsp::DelayLine`) that can widen genuinely mono-compatible material, clearly documented as the one exception to Firmament's otherwise-provable mono-sum guarantee.
- `docs/manual.md`: a full user manual (what Firmament is, where it sits in a symphonic-metal chain, signal flow, complete parameter reference, mixing tips).
- Broadened Catch2 test suite (24 -> 49 test cases): sample-rate sweeps (44.1-192 kHz), extreme/randomised parameter automation, mono/stereo bus-configuration coverage (including direct `isBusesLayoutSupported()` acceptance/rejection tests), and long-run NaN/Inf stability sweeps, alongside dedicated coverage for every M1 DSP addition above.

### Changed

- `PluginEditor`: extended the v0.1-style functional editor with controls for every new M1 parameter (`Low Width` knob, `Auto Mono Safety`/`Haas Mode` toggles, `Haas Time` knob). A custom vector-drawn LookAndFeel and a visible correlation/phase meter widget remain M3 scope.
- `docs/architecture.md`: updated signal-flow diagram and per-stage documentation for the M1 signal path.

### Fixed

- Corrected a documentation inaccuracy inherited from the v0.1 bootstrap: `juce::dsp::LinkwitzRileyFilter`'s dual-output low/high bands sum to a **flat-magnitude allpass**, not an exact reconstruction of the input (per JUCE's own class documentation, confirmed empirically) - the original "Bass Mono" feature was unaffected by this (it only ever discards the low band rather than re-summing), but the documentation claim was corrected before it could mislead the new multiband-width design.
