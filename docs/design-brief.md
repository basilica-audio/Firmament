# Firmament — Design Brief v2 (binding; supersedes v1 entirely)

The orchestral/heavy-mix stereo widener and imager, rebuilt against the documented behavior of
the category-defining units and techniques — the vinyl-mastering/mastering-forum consensus on
bass-mono crossover frequency, the 4th-order Linkwitz-Riley (LR-4) crossover as the de facto
professional standard, the Haas/precedence-effect timing window, the documented mono-fold-down
cost of delay-based widening versus allpass decorrelation (Ozone Imager's dual Stereoize
modes), professional phase-correlation-meter ballistics/threshold conventions, and per-band
correlation-safety precedent (KERN WIDE, HoRNet ZeroWidth, In The Mix Bandwidth) — assuming the
post-fix state of GitHub issues #12 (stale crossover state while bass-mono disabled) and #13
(Auto Mono Safety toggle stepping) as the M1 baseline this brief builds on.
Research-driven rewrite: every default below is sourced (see
`docs/research-notes.md`). **No brand or person names in parameters, UI or marketing copy** —
v1's naming was already generic ("Width", "Bass Mono Freq", "Auto Mono Safety", "Haas Mode")
and stays that way; the manual may cite public sources.

## Why v1 falls short (the two core corrections)

1. **Haas Mode is the only alternative widening technique, and it is a pure fixed delay — the
   worse of two well-documented options for widening near-mono material.** Delaying one channel
   and folding to mono produces "deep notches at regular frequency intervals, spaced at
   1/delay Hz apart" (comb filtering); the researched alternative, allpass-based decorrelation,
   "preserves magnitude spectrum with mild spectral ripple on mono fold-down (1 to 2 dB)"
   instead. This is exactly why iZotope's Ozone Imager ships **two** distinct mono-to-stereo
   techniques (Haas-style delay and velvet-noise decorrelation) rather than one. v1 documents
   Haas Mode's mono-sum cost honestly (good) but offers no lower-cost alternative when a user
   specifically wants width from mono-compatible material without that cost.
2. **Auto Mono Safety is one global, broadband, hair-trigger gain — real safety-net designs are
   calmer and frequency-aware.** v1 reacts to *any* correlation reading below exactly 0.0 and
   integrates over 200 ms; documented professional correlation-meter practice explicitly treats
   "the occasional small deviation into the negative side" as insignificant and integrates over
   a much steadier ~600 ms. Separately, the researched reference class for correlation-driven
   width safety (KERN WIDE's per-ERB-band correlation constraint, HoRNet ZeroWidth's per-band
   correlation learning, In The Mix Bandwidth's per-band approach) treats mono-safety as a
   **per-band** problem, not one broadband number — meaningful because Firmament's own
   bass-mono crossover already splits the signal into exactly the two bands (low/high) a safety
   net could reason about independently, and currently doesn't.

## Topology (unchanged M/S core; two additive stages, not a restructure)

```
input -> Encode M/S -> Side -> [Bass Mono off] Width (0-200%) -------------\
                             \-> [Bass Mono on]  Low Width (low band)       |
                                                  Width (high band) --------/
                             --> [optional] Auto Mono Safety (broadband or per-band)
                          Mid (untouched) ------------------------------------+
                                                                               v
                                                                 Decode M/S --> [optional]
                                                                 Decorrelate OR Haas Mode
                                                                 (mutually exclusive)
                                                                 --> Output trim --> output
```

The Mid/Side mono-compatibility invariant (`docs/architecture.md`) is **not** touched or
weakened by any v2 change — Width/Low Width/Auto Mono Safety (broadband or per-band) still only
ever scale Side, never Mid. Decorrelate is new, post-decode, alongside Haas Mode; it is the
*second* deliberate, documented exception to the mono-sum guarantee (Haas Mode was the first,
and stays one) — not a replacement for the guarantee, an alternative widening tool for when a
user wants some of what Haas Mode offers at a fraction of the mono-fold-down cost.

## Module specifications (authentic behaviors, generically named)

### Bass Mono Freq / Width / Low Width (validated, minor range extension)

- Range/default/skew (0-500 Hz, 0 = off, 0.4 skew favoring the low end) and the 0-200% Width/Low
  Width convention are **directly confirmed** by research: the 80-180 Hz consensus cluster
  (mastering-forum consensus, Brainworx Mono Maker's commonly-cited 160-180 Hz range) sits
  comfortably inside v1's existing range, and the manual's own "80-200 Hz typical" copy already
  matches it. **No change to defaults or the manual's existing framing.**
- **Reasoned, low-confidence extension**: raise the Bass Mono Freq range ceiling from 500 Hz to
  **600 Hz**, matching the Waves S1 Shuffle control's documented ~600 Hz convention for
  low-frequency-stereo-width control (the rationale — "the ear is less sensitive to stereo bass
  effects" — is exactly Low Width's own rationale). This is the single lowest-confidence change
  in this brief (indirect, third-party-summarized source, not a primary manual) and is flagged
  as such in the manual/CHANGELOG rather than presented as a strong correction.
- Filter topology (`juce::dsp::LinkwitzRileyFilter`, 4th-order/24 dB-oct) is **directly
  confirmed** as the professional-standard crossover order/topology and needs no change.

### Auto Mono Safety — ballistics, dead-zone, floor exposure, per-band mode

- **Ballistics**: internal `correlationTimeConstantSeconds` moves from 200 ms toward **300 ms**
  — a reasoned compromise, not the full ~600 ms documented for a pure *display* correlation
  meter (Auto Mono Safety is a live control-loop input as well as (eventually) a meter feed, so
  staying meaningfully faster than a passive display is deliberate) but a clear step toward the
  documented professional convention and away from v1's comparatively twitchy 200 ms. Not
  user-exposed — internal constant, like the crossover/Haas smoothing cadence already are.
- **Dead-zone (new internal behavior)**: Auto Mono Safety no longer starts attenuating at
  literally any negative correlation. A new internal constant, `autoMonoSafetyDeadZone =
  -0.10`, means correlation in `[-0.10, 1.0]` still yields full (1.0) safety gain, and the
  existing linear ramp toward the floor now runs from `-0.10` down to `-1.0` rather than from
  `0.0` down to `-1.0`. Directly sourced: "the occasional small deviation into the negative side
  is usually insignificant." `-0.10` itself is a reasoned magnitude (no source gives an exact
  number) chosen to be clearly inside "occasional small deviation" while still catching a
  genuinely problematic, sustained negative reading well before `-1.0`.
- **`autoMonoSafetyFloorDb`** (new parameter, float, **-24 to 0 dB, default -9.1 dB**): exposes
  the previously-hardcoded 0.35 linear floor gain (≈ -9.1 dB) as a user-adjustable minimum Side
  gain reached at full out-of-phase correlation. Default is chosen to reproduce v1's existing
  behavior exactly (bit-exact at default, per the versioning/migration rules below) — the
  -9.1 dB value itself remains a reasoned choice (no source publishes an exact floor-gain
  number for this category of feature); v2 makes it adjustable rather than claiming a better
  number exists.
- **`autoMonoSafetyMultiband`** (new parameter, bool, **default off**): when on *and* Bass Mono
  Freq is engaged (> 0 Hz), Auto Mono Safety computes and applies the correlation-derived gain
  **independently for the low and high bands already split out by the bass-mono crossover**,
  instead of one broadband estimate scaling both. When Bass Mono Freq is off, or this parameter
  is off, behavior is unchanged (single broadband estimate, as in v1/v0.1.1). Sourced to the
  *principle* documented across KERN WIDE/HoRNet ZeroWidth/In The Mix Bandwidth (mono-safety
  reasoning per-band beats broadband); the specific 2-band mapping onto Firmament's existing
  low/high split is reasoned (no source publishes an exact 2-band scheme — the closest sources
  use 7 or 40 bands for a dedicated multiband imager, a materially different scope than
  Firmament's existing 2-band bass-mono architecture). Off by default because it changes Auto
  Mono Safety's audible behavior whenever both it and Bass Mono Freq are engaged together — an
  explicit opt-in, matching the suite's existing "default reproduces prior behavior" convention.

### Decorrelate (new parameter pair, alternative to Haas Mode for widening near-mono material)

- **`decorrelateEnabled`** (new parameter, bool, **default off**) and **`decorrelateAmount`**
  (new parameter, float, **0-100%, default 50%**): engages a post-M/S-decode allpass-based
  decorrelation stage on the Right channel (mirroring where Haas Mode's delay sits in the
  chain), trading a much smaller, well-documented mono-fold-down cost ("mild spectral ripple...
  typically less than 1 to 2 dB") for a gentler width effect than Haas Mode's precedence-effect
  delay produces. `decorrelateAmount` scales the allpass network's effective phase-shift depth;
  the 0-100%/50% convention matches Width/Low Width's existing scale-parameter style rather than
  inventing a new one. Exact allpass topology (stage count/coefficient spacing) is
  **implementation-reasoned** — no source publishes an exact filter design for this category of
  effect, only the *principle* (spread, irregular phase shift beats a single time offset for
  mono-fold-down cost) and the *comparative magnitude* (1-2 dB ripple vs. deep comb notches),
  both of which are directly sourced.
- **Mutual exclusivity with Haas Mode**: if both `decorrelateEnabled` and `haasEnabled` are on,
  Decorrelate takes effect and Haas Mode's delay line is held at 0 samples (bypassed) for as
  long as Decorrelate is engaged — documented behavior, not a silent conflict. Rationale: both
  operate on the same post-decode Right channel for the same underlying goal (width from
  near-mono material); stacking them would make the *already*-approximate mono-fold-down cost
  of each impossible to reason about independently, undermining the entire point of offering
  the lower-cost alternative. A user who wants both effects' character can still automate
  between them.
- **Manual/honesty framing**: Decorrelate is presented as "gentler, more mono-tolerant" not
  "mono-safe" — it is still, like Haas Mode, an explicit, documented exception to Firmament's
  `left + right == 2 * mid` invariant, just a smaller one. The exact invariant (Width/Low
  Width/Auto Mono Safety, broadband or multiband) remains completely unaffected by either
  Decorrelate or Haas Mode, both of which sit strictly after M/S decode.

### Width, Low Width, Output, Haas Time (unchanged)

- Width/Low Width's 0-200% range and multiplicative-scale behavior is directly validated by the
  Waves S1 Widener/Shuffle precedent (Section 7 of the research notes) — no change.
- Output's -24 to +24 dB trim range is a generic mix-utility control outside this deep-dive's
  category-defining-behavior scope — no change.
- Haas Time's 0-40 ms range and 20 ms default are directly validated by the researched
  Haas-window literature (5-35 ms optimal, 10-30 ms "Haas Window", ~40 ms echo-perception
  ceiling) — no change to Haas Mode itself, which stays exactly as documented in v1/v0.1.1
  (including the "Fixed for v0.1.1" state-smoothing/live-filter-state fixes from issues #12/#13
  this brief assumes are already merged).

## Factory Presets (M2 preset system — proposed, not yet implemented)

1. **Open Strings** — Width=140%, BassMonoFreq=110Hz, LowWidth=0%, AutoMonoSafety=on
   (Multiband=off), Output=0dB. The primary "widen the orchestral/choir bus, keep the low end
   centered" default — BassMonoFreq sits in the researched 80-180Hz consensus center.
2. **Choir Bloom** — Width=170%, BassMonoFreq=130Hz, LowWidth=15% (a little low-end air, not
   full mono), AutoMonoSafety=on, FloorDb=-12dB (a firmer safety net for a wide, exposed
   setting), Output=-1dB.
3. **Doubled Rhythm Glue** — Width=125%, BassMonoFreq=150Hz, LowWidth=0%, AutoMonoSafety=off
   (manual, predictable material), Output=0dB. The manual's own documented "110-140%" doubled-
   guitar-bus use case.
4. **Master Bus Bass Mono** — Width=100% (pass-through above the crossover), BassMonoFreq=90Hz,
   LowWidth=0%, AutoMonoSafety=on, Multiband=on (protects the wide highs without dulling the
   already-centered, safe low end), Output=0dB. The manual's documented master-bus use case.
5. **Automated Width Safety Net** — Width=150%, BassMonoFreq=100Hz, LowWidth=0%,
   AutoMonoSafety=on, FloorDb=-9.1dB (v1/v0.1.1 default floor), Multiband=off, Output=0dB. For
   busses with automated/unpredictable Width where a human isn't constantly monitoring — the
   manual's own documented Auto Mono Safety use case.
6. **Mono-Safe Air** (Decorrelate) — Width=100%, BassMonoFreq=off, DecorrelateEnabled=on,
   DecorrelateAmount=35% (subtle), HaasMode=off, Output=0dB. Width from near-mono source
   material (a mono-tracked lead, a narrow pad) with the lower-cost mono-fold-down technique.
7. **Wide Pad, Full Precedence** (Haas) — Width=100%, BassMonoFreq=off, HaasMode=on,
   HaasTimeMs=22ms, DecorrelateEnabled=off, Output=-1dB. The strong, well-known
   precedence-effect widening technique for material where mono translation is a secondary
   concern (a synth pad layer, a transition/breakdown effect) — HaasTimeMs sits centrally in the
   researched 10-30ms "Haas Window."
8. **Extreme Width (special effect)** — Width=200%, BassMonoFreq=120Hz, LowWidth=0%,
   AutoMonoSafety=on, FloorDb=-15dB (a hard safety net for an extreme setting), Output=-3dB. The
   manual's own documented "200% is a special-effect setting, not a default" guidance,
   packaged with a firm safety net rather than left unprotected.
9. **Subtle Openness** — Width=115%, BassMonoFreq=100Hz, LowWidth=0%, AutoMonoSafety=off,
   Output=0dB. A barely-there width lift for sources that just need a hint of stereo interest
   without drawing attention to the processing.

Preset system implementation (state schema, factory-vs-user storage, recall UI) is M2 scope and
out of this brief's boundary — the above are intent + rough settings for whoever implements M2
to encode.

## Guarantees & tests (Catch2; keep all still-valid v1/v0.1.1 cases, adapt/extend the rest)

1. **Mono-sum invariant, still exact for the pre-decode chain**: `left + right == 2 * mid`
   holds bit-exactly across Width/Low Width/Auto Mono Safety (broadband **and** the new
   multiband mode) at any setting, including the new `autoMonoSafetyFloorDb` and
   `autoMonoSafetyMultiband` parameters swept to their extremes — direct extension of the
   existing "never breaks the mono-sum invariant" tests to cover the two new parameters.
2. **Auto Mono Safety dead-zone**: correlation values in `[-0.10, 1.0]` produce Side gain
   bit-identical to full bypass (1.0, modulo the existing on/off smoothing ramp); only
   correlation below `-0.10` produces measurably reduced gain — proves the dead-zone is
   implemented, not just documented.
3. **Auto Mono Safety floor is user-controlled**: sweeping `autoMonoSafetyFloorDb` from -24 to
   0 dB, with correlation pinned at -1.0 (fully out-of-phase), measures Side gain landing within
   tolerance of the corresponding linear floor at every tested value; default (-9.1 dB) measures
   within tolerance of v1's hardcoded 0.35 floor — proves the parameter both works and preserves
   the old default bit-for-bit.
4. **Multiband Auto Mono Safety independence**: with `autoMonoSafetyMultiband` on and Bass Mono
   Freq engaged, feed a signal that is out-of-phase only above the crossover (in-phase below
   it); assert the low band's measured gain stays at 1.0 (safety doesn't fire) while the high
   band's measured gain drops toward the floor — the core proof that per-band reasoning beats
   broadband. A matching test with `autoMonoSafetyMultiband` off on the same signal asserts the
   single broadband estimate *does* pull both bands down together (regression guard proving the
   parameter actually changes behavior).
5. **Ballistics regression guard**: the correlation estimate's settling time (time to reach a
   defined fraction of its final value after a correlation step) measures within tolerance of
   the new 300 ms time constant, not the old 200 ms one — direct proof the ballistics change
   landed, not just a comment update.
6. **Decorrelate mono-fold-down cost, measured**: with `decorrelateEnabled` on at a swept
   `decorrelateAmount`, sum the processed L/R to mono and compare the mono-summed magnitude
   spectrum against the pre-Decorrelate spectrum; assert peak deviation stays within a small,
   defined dB tolerance (the "1-2 dB ripple" documented ballpark) — and, as a contrasting
   regression guard, assert Haas Mode's equivalent mono-summed spectrum shows deviation an order
   of magnitude larger (the documented comb-filter-notch behavior), proving Decorrelate is
   measurably gentler than Haas Mode on mono fold-down, not just verbally described as such.
7. **Decorrelate/Haas mutual exclusivity**: with both `decorrelateEnabled` and `haasEnabled` on,
   assert the Haas delay line's applied delay is 0 samples (bypassed) and the output matches
   Decorrelate-only processing bit-exactly — proves the documented precedence rule is actually
   enforced, not just described in the manual.
8. **Decorrelate off is a transparent passthrough** (mirrors Haas Mode's existing "off
   (default) is a fully transparent passthrough" test): with `decorrelateEnabled` off (default),
   output nulls against Decorrelate-disabled reference processing to the same -90 dBFS bound
   used for the existing Haas/unity-round-trip tests.
9. **Bass Mono Freq range extension**: parameter range/skew tests updated for the new 0-600 Hz
   ceiling; existing crossover-behavior tests (forced-mono-below-crossover, magnitude
   preservation, live-filter-state-on-re-engagement from the #12 fix) re-run parametrized across
   the extended range, not just the old 0-500 Hz span.
10. **Existing v1/v0.1.1 guarantees, retained**: unity M/S round-trip null test; mono downmix
    invariance across Width/multiband/Auto Mono Safety; Linkwitz-Riley magnitude-preservation
    (not identity) on multiband re-sum; latency stays exactly 0 across every new parameter
    combination (Decorrelate's allpass stage must be a zero-latency IIR structure, like the
    existing crossover, not an FIR/oversampled one — a dedicated latency test asserts this
    explicitly); NaN/Inf sweep across every parameter including all five new ones; state
    round-trip including tolerant v1/v0.1.1-to-v2 import (unknown/removed param IDs ignored,
    the five new IDs default to their v2 defaults — which reproduce v1/v0.1.1 behavior exactly
    — when absent from a loaded older state); bus-layout coverage (mono/stereo) unchanged;
    sample-rate sweep (44.1-192 kHz) extended to cover the new parameters; long-run NaN/Inf
    stability under randomised automation of all five new parameters together with the existing
    ones (~1000 blocks, matching the existing long-run test's scale).

## Honesty & framing

- `docs/research-notes.md` ships the sourced findings (quotes + URLs) — the voicing is
  **research-derived from public manuals, developer/product documentation, mastering-forum and
  trade-press consensus, and acoustics/psychoacoustics literature**, not measured against any
  commercial widener plugin's actual audio output, and no proprietary DSP algorithm from Waves,
  iZotope, Brainworx, KERN, or HoRNet was inspected, decompiled, or approximated. Say so in the
  manual.
- Numbers fall into three explicit classes and the manual/comments should keep saying which:
  (a) **directly sourced, multi-source-corroborated** (the 80-180 Hz bass-mono consensus
  center; LR-4/24 dB-oct as the professional crossover standard; the 5-35/10-30 ms Haas window
  and ~40 ms echo ceiling; the correlation-meter `[-1,1]` scale and ~600 ms integration
  convention; the comb-filter-vs-allpass-ripple comparative finding), (b) **directly sourced,
  single-cluster or indirect** (Brainworx Mono Maker's 160-180 Hz figure, reconstructed from
  third-party summaries of an image-scanned manual; Waves S1 Shuffle's ~600 Hz convention,
  reconstructed from third-party review sources rather than Waves' own manual — both flagged as
  lower-confidence in `docs/research-notes.md`), and (c) **research-derived/reasoned** (the
  300 ms ballistics compromise, the -0.10 dead-zone threshold, the -9.1 dB default floor, the
  Decorrelate allpass topology and its 0-100% amount curve, the exact 2-band mapping for
  multiband safety) where a source establishes the *principle* but not an exact number for
  Firmament's specific parameter space.
- The manual explicitly reframes Decorrelate as "gentler, not mono-safe" — it is a second,
  smaller exception to the mono-sum invariant, not a fix that makes Haas Mode's trade-off
  obsolete. Users who need the *exact* mono-sum guarantee still need Width/Low Width/Auto Mono
  Safety alone, with both Decorrelate and Haas Mode off.
- Out of scope for v2 (explicitly): a true per-ERB-band (7- or 40-band) imager/safety engine
  matching KERN WIDE/HoRNet ZeroWidth's full scope (Firmament's existing 2-band bass-mono split
  is the only band boundary v2 reasons about); auto-EQ/auto-Width "learning" features (HoRNet
  ZeroWidth's adaptive calibration); a visible correlation/phase-meter GUI widget (the
  underlying value has been fully computed since M1 — this is still M3 scope, unchanged); any
  change to Haas Mode's own algorithm (it stays exactly as fixed for v0.1.1). These are
  candidates for a later milestone, tracked as issues.

## Versioning

Ships as **v0.2.0** (breaking parameter changes are fine pre-1.0 — `decorrelateEnabled`,
`decorrelateAmount`, `autoMonoSafetyFloorDb`, and `autoMonoSafetyMultiband` are new parameter
IDs; the Bass Mono Freq range ceiling change from 500 to 600 Hz is a range/skew refinement,
which `docs/params/ParameterIds.h`'s own frozen-ID rule explicitly permits pre-1.0). State
migration = tolerant import: v1/v0.1.1 states load cleanly with all four new parameters
defaulting to values that reproduce old behavior exactly (Decorrelate off, Multiband off, Floor
at -9.1 dB matching the old hardcoded value), no crash on unknown/missing IDs, consistent with
the `AudioProcessorValueTreeState` tolerant-load behavior already relied on elsewhere in the
suite. CHANGELOG documents the Decorrelate addition and the Auto Mono Safety ballistics/dead-
zone/per-band changes prominently as the headline v0.2.0 items, and calls out the Bass Mono
Freq range extension explicitly as the lowest-confidence, most-reasoned change in this release.
