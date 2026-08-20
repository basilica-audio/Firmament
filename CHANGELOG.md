# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.0] - 2026-08-20

The M3 GUI release. The interim functional editor becomes the suite's fully
vector-drawn, fully accessible surface, and the correlation metering the engine
has exported since v0.1 finally gains a visual consumer. No DSP changes - the
audio path is bit-identical to v0.3.1.

### Added

- **M3 custom vector editor** (issue #4, ported from Miserere's merged M3
  implementation, basilica-audio/miserere PR #31): the interim slider/dropdown
  editor is replaced by the suite's fully vector-drawn black/gold surface —
  pointer knobs with engraved scale rings (choice parameters as detented knobs
  announcing the choice *name*), lamp toggles, EB Garamond typography embedded
  via BinaryData (OFL licensed), and five signal-flow section panels (Width /
  Bands / Mono Safety / Widen / Output). No photoreal PNG assets; everything is
  drawn at runtime with `juce::Graphics`/`juce::Path`.
- **Correlation needle meters, finally wired to the GUI**: two vector needle
  meters (input and output) driven by the engine's per-block correlation
  metering via relaxed atomics and a 30 Hz GUI timer with one-pole ballistics —
  the DSP-side meter surface has existed since v0.1/v0.3.0, this is its first
  visual consumer.
- **Accessible parameter surface** (WCAG 2.1 AA): every control keyboard-
  operable (WAI-ARIA stepping: Arrow 1%, Shift+Arrow fine, PageUp/Down 10%,
  Home/End extremes), visible focus rings on all custom-painted controls,
  name/value/role for every knob/toggle/meter (unit-suffixed accessible values,
  read-only meter values), section panels as accessibility focus containers
  (grouped AT navigation without trapping Tab), and WCAG-contrast unit tests
  pinned to the exact rendered colour pairs. New test suites:
  `tests/gui/EditorAccessibilityTests.cpp`, `EditorLayoutTests.cpp`,
  `BasilicaLookAndFeelContrastTests.cpp`, `CorrelationMeterTests.cpp`.

## [0.3.1] - 2026-07-31

Race-fix patch release.

### Fixed

- **Cross-thread races around linear-phase reconfiguration and latency reporting** (PR #25, ThreadSanitizer-confirmed). `LinearPhaseCrossover::prepare()` (host's `prepareToPlay()` thread) raced the 50 ms timer-driven `serviceMessageThreadUpdates()` (JUCE message thread) — the code even carried the false assumption "prepareToPlay runs on the message thread" in a comment. TSan showed 10-11 races per run on the unfixed engine's pending-command hand-off; fixed via a message-thread mutex in `LinearPhaseCrossover`, an atomic flag in `FirmamentEngine`, and serialized `setLatencySamples()` call sites. Clean under TSan post-fix. New regression guard: `tests/CrossThreadReprepareTests.cpp`; the allocation guard gained an elision-safe self-test.

## [0.3.0] - 2026-07-26

SOTA DSP pass (binding brief `.scaffold/research/2026-07-25-sota/brief-firmament.md`;
math sources in `docs/research-notes.md` Section 9). Headline: every widening
mode except Haas is now provably mono-safe by construction - the velvet
decorrelation modes fold down to mono bit-for-bit identically to the input.
Every new parameter defaults to exact v0.2.0 behaviour, verified bit-exactly
against a frozen v0.2.0 state fixture (`tests/StateTests.cpp`).

### Added

- **Velvet-noise decorrelation modes** (`decorrelateMode`: Classic / Velvet
  Dense / Velvet Sparse): the published DAFx-18 "Optimized Velvet-Noise
  Decorrelator" tap pairs (OVN30/OVN15, transcribed as data from the paper's
  open-access tables) as sparse zero-latency FIRs, applied to the Side signal
  only (`S' = (1-d)S + d*(VND_A(L)-VND_B(R))/2`, Mid dry) so the mono sum is
  invariant by construction - measured fold-down dip ~0.0000003 dB vs 16 dB
  for the unguarded Haas control on the same near-mono program. Tap times are
  rescaled ms-true to the session rate and renormalised to unit energy.
  Classic remains the bit-exact v0.2.0 R-only allpass cascade and the default.
- **Bass Mono Mode selector** (`bassMonoMode`: Classic / Phase Matched /
  Linear Phase): Phase Matched passes Mid through the companion AP2
  (Q = 1/sqrt(2)) derived from the same prewarped coefficients as the LR4
  crossover (measured inter-path phase error ~5e-6 degrees, negative control
  179.8 degrees); Linear Phase switches the Side split to a Kaiser-sinc FIR
  complementary crossover (perfect reconstruction ~7e-9 peak residual, 96 dB
  side attenuation below 0.5*fc at Low Width 0) via juce::dsp::Convolution
  with message-thread-only kernel handoff - the codebase's first
  nonzero-latency path (N/2 = 2048 samples @48 kHz, reported dynamically via
  setLatencySamples and verified against the measured impulse offset at
  44.1/48/96/192 kHz).
- **3-band width** (`highSplitFreq` 0 + 500-8000 Hz, `highWidth` 0-200%): a
  second Side-path LR4 above the bass-mono split with the standard 3-way
  Linkwitz-Riley low-band AP2 flat-sum discipline (band sum flat within
  +/-0.1 dB, measured 0.000003 dB); sentinel 0 = off is bit-identical to the
  2-band path.
- **Dynamic safety ballistics** (`safetyMode`: Smooth / Dynamic): a fast
  30 ms correlation detector driving the unchanged v0.2.0 attenuation map
  through a dedicated asymmetric one-pole (attack 5 ms / release 250 ms as
  times-to-90%-settling), per band in Multiband mode. A binding energy-gate
  rule now makes every correlation estimate decay to 0 under silence
  (display meters and guard detectors alike) - fixes both the "meter shows
  +/-1 on silence" trap and the guard latching -1 after an anti-phase burst
  into silence.
- **Equal-power width compensation** (`widthComp`, off by default): post-decode
  makeup g = 1/sqrt(a^2+b^2) from the broadband Width, RMS constant within
  +/-0.5 dB across width 0-200% on decorrelated program.
- **Mono Audition** (`monoAudition`): post-everything (L+R)/2 monitor switch,
  50 ms crossfaded, excluded from factory presets.
- **Meter surface**: per-band input correlation (including the new third
  band) and a broadband output (post-processing) correlation exported as
  atomics for the M3 GUI; all energy-gated.
- **State-schema versioning**: `stateVersion = 2` stamped on save; absent =
  version 1 (v0.1.x/v0.2.0), which loads tolerantly with all new parameters
  at neutral defaults (the neutral-default design is the migration).
  Migration verified two ways: tolerance-0 same-binary null, and
  <= -140 dBFS cross-version null against a frozen v0.2.0 reference render
  checked into `tests/fixtures/`.
- **Factory presets** (additive; the 10 v0.2.0 presets are untouched):
  `Velvet Width`, `Mastering: Linear Phase Bass Mono`, `Three-Band Imager`.
- **AllocationGuardTests**: thread-scoped global operator new/delete override
  asserting zero audio-thread heap allocations under every mode combination,
  including Linear Phase and mode switches mid-run, plus the 16384-sample
  oversized-block chunk guard.

### Changed

- **Haas polish**: delay interpolation Linear -> Lagrange3rd (fractional-delay
  HF response improves; integer-sample delays, including the 20 ms default at
  48 kHz = 960 samples, are unchanged) and per-sample smoothed delay-time
  application (kills the automation pitch-zipper).
- **Toggle discipline**: `haasEnabled` and `decorrelateEnabled` are now 50 ms
  crossfades instead of instant per-block gates, matching the Auto Mono
  Safety toggle discipline.
- Version bumped to 0.3.0; 7 new parameters, all defaulting to exact v0.2.0
  behaviour; no existing parameter IDs, ranges, or defaults changed.

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
