#pragma once

// Central definition of all AudioProcessorValueTreeState parameter IDs for
// Firmament. See docs/architecture.md for the corresponding signal-flow
// diagram.
//
// FROZEN AS OF THE v0.1 PARAMETER LAYOUT:
// Parameter IDs below must NEVER change once shipped - saved sessions and
// presets persist the APVTS state keyed by these string IDs, and renaming or
// removing one would silently break every user's saved state. Ranges,
// defaults, and skew MAY still be refined during voicing/tuning milestones;
// only the IDs themselves are frozen.
namespace ParamIDs
{
    // Mid/Side width scale applied to the Side channel. 100% is an unmodified
    // pass-through of the encoded M/S signal (unity, decodes back to the
    // original L/R); 0% collapses the Side channel to silence (mono); up to
    // 200% doubles the Side channel's amplitude for an exaggerated image.
    inline constexpr auto width = "width";

    // Crossover frequency (Hz) for the optional bass-mono stage: Side-channel
    // content below this frequency is removed (forced to mono via the Mid
    // channel), Side content above it keeps the Width-scaled stereo image.
    // 0 Hz means the stage is fully bypassed (no forced mono at any
    // frequency).
    inline constexpr auto bassMonoFreq = "bassMonoFreq";

    // Output trim, applied after M/S decode back to L/R.
    inline constexpr auto output = "output";

    // M1 additions below (added post-v0.1 bootstrap, still pre-1.0 - IDs are
    // permanent from the moment they ship in a tagged release, same rule as
    // the three above).

    // Width scale (0-200%, same convention as `width`) applied to the Side
    // band *below* the BassMonoFreq crossover, independently of `width`
    // (which - when the crossover is engaged - now applies only to the band
    // *above* it). Only audible while bassMonoFreq > 0; default 0% exactly
    // reproduces the v0.1 "bass mono forces the low band to silence"
    // behaviour, so existing sessions/presets are unaffected. See
    // FirmamentEngine's multiband-width comments and docs/architecture.md.
    inline constexpr auto lowWidth = "lowWidth";

    // Auto Mono Safety: on/off. When engaged, a running correlation estimate
    // of the plugin's input (see `correlation` discussion in
    // FirmamentEngine.h) is used to automatically attenuate the Side channel
    // when the input is heavily out-of-phase, independent of the Width/Low
    // Width settings - a safety net against a widened signal collapsing
    // destructively on mono fold-down. Off by default (v0.1 behaviour is
    // unchanged unless explicitly enabled).
    inline constexpr auto autoMonoSafety = "autoMonoSafety";

    // Haas Mode: on/off. When engaged, the Right channel is delayed by
    // HaasTimeMs relative to Left (after M/S decode), trading the M/S
    // width-scaling model's exact mono-sum guarantee for a stronger,
    // psychoacoustic (precedence-effect) sense of width. Off by default.
    inline constexpr auto haasEnabled = "haasEnabled";

    // Haas Mode delay time in milliseconds (0-40 ms), only audible while
    // haasEnabled is on.
    inline constexpr auto haasTimeMs = "haasTimeMs";

    // v0.2.0 additions below (docs/design-brief.md's research-driven
    // deep-dive). All four new IDs default to values that reproduce v0.1.1
    // behaviour exactly, so a v0.1/v0.1.1 state loads cleanly with these
    // simply absent (APVTS's tolerant load leaves an unmentioned parameter
    // at whatever it already has - its ParameterLayout default on a fresh
    // instance) - see docs/design-brief.md's "Versioning" section.

    // Auto Mono Safety's minimum Side gain at full out-of-phase correlation
    // (-24 to 0 dB, default -9.1 dB - reproduces v0.1.1's hardcoded 0.35
    // linear floor). See FirmamentEngine.h's Auto Mono Safety comment for
    // the dead-zone/ballistics changes this floor now interacts with.
    inline constexpr auto autoMonoSafetyFloorDb = "autoMonoSafetyFloorDb";

    // When on *and* bassMonoFreq is engaged (> 0 Hz), Auto Mono Safety
    // computes and applies its correlation-derived gain independently for
    // the low/high bands already split out by the bass-mono crossover,
    // instead of one broadband estimate scaling both. Off by default (no
    // change to v0.1.1 behaviour unless explicitly enabled); has no effect
    // while bassMonoFreq is off (single band only) or Auto Mono Safety
    // itself is off.
    inline constexpr auto autoMonoSafetyMultiband = "autoMonoSafetyMultiband";

    // Decorrelate: on/off. An alternative, gentler widening technique to
    // Haas Mode for near-mono material - a post-M/S-decode allpass-based
    // decorrelation of the Right channel, trading a much smaller documented
    // mono-fold-down cost (mild spectral ripple) for a gentler width effect
    // than Haas Mode's precedence-effect delay. Off by default. Mutually
    // exclusive with haasEnabled: whenever both are on, Decorrelate takes
    // effect and Haas Mode's delay is held at 0 samples - see
    // FirmamentEngine.h.
    inline constexpr auto decorrelateEnabled = "decorrelateEnabled";

    // Decorrelate's effective phase-shift depth (0-100%, default 50%), only
    // audible while decorrelateEnabled is on.
    inline constexpr auto decorrelateAmount = "decorrelateAmount";

    // v0.3.0 additions below (binding brief
    // .scaffold/research/2026-07-25-sota/brief-firmament.md). All seven new
    // IDs default to values that reproduce v0.2.0 behaviour exactly, so a
    // v0.1.x/v0.2.0 state loads cleanly with these simply absent (APVTS's
    // tolerant load leaves an unmentioned parameter at its ParameterLayout
    // default) - verified bit-exactly by tests/StateTests.cpp's migration
    // null tests. See docs/manual.md for the user-facing behaviour of each.

    // Decorrelate topology: Classic (v0.2.0 R-only 4-stage allpass cascade,
    // default) / Velvet Dense (OVN30 symmetric velvet-noise pair) / Velvet
    // Sparse (OVN15 pair). All paths always process; switching crossfades
    // over 50 ms.
    inline constexpr auto decorrelateMode = "decorrelateMode";

    // Bass-mono topology (active only while bassMonoFreq > 0): Classic
    // (v0.2.0 LR4-on-Side, Mid untouched, default) / Phase Matched (Mid
    // through the LR4's companion AP2 - seam-free recombination, zero
    // latency) / Linear Phase (FIR complementary split, N/2 samples of
    // reported latency).
    inline constexpr auto bassMonoMode = "bassMonoMode";

    // Second Side-path crossover frequency for 3-band width (0 = off
    // sentinel, mirroring bassMonoFreq; 500-8000 Hz active range, internally
    // clamped to >= 2 * bassMonoFreq).
    inline constexpr auto highSplitFreq = "highSplitFreq";

    // Width scale (0-200%) for the band above highSplitFreq; inert while
    // highSplitFreq is 0. Default 100% (unity).
    inline constexpr auto highWidth = "highWidth";

    // Auto Mono Safety response: Smooth (v0.2.0 static map on the 300 ms
    // estimate, default) / Dynamic (30 ms fast detector + 5 ms/250 ms
    // compressor-style gain ballistics).
    inline constexpr auto safetyMode = "safetyMode";

    // Equal-power width compensation (post-decode makeup so perceived level
    // stays constant across Width settings). Off by default - the mastering
    // convention "width changes side level only" remains the default.
    inline constexpr auto widthComp = "widthComp";

    // Mono audition: post-everything L = R = (L+R)/2 monitor switch. Off by
    // default; excluded from factory presets (a monitor control, not a
    // sound).
    inline constexpr auto monoAudition = "monoAudition";
}
