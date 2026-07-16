# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
