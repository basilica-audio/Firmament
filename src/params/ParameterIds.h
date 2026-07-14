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
}
