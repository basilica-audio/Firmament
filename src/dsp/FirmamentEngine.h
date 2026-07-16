#pragma once

#include <juce_dsp/juce_dsp.h>

#include "MidSideCodec.h"

#include <array>

// The complete Firmament signal path, independent of juce::AudioProcessor so
// it can be exercised directly by unit tests without instantiating a full
// plugin (see tests/EngineTests.cpp). Owns all DSP state; every filter is
// allocated in prepare() and never reallocated on the audio thread.
//
// Signal flow (see docs/architecture.md for the full diagram):
//
//   L/R -> encode M/S -> [multiband] Width scale on Side -> [optional]
//       Auto Mono Safety gain on Side -> decode M/S -> L/R -> [optional]
//       Haas Mode delay on Right -> Output trim
//
// Multiband width: when the bass-mono crossover is engaged (BassMonoFreq >
// 0), the derived Side stream is split into a low and a high band by the
// same juce::dsp::LinkwitzRileyFilter used for the v0.1 "bass mono" feature,
// and each band is scaled by its own width control - LowWidth below the
// crossover, Width above it - before being summed back together. Scaling a
// band by a constant commutes exactly with the (linear) filtering that
// produced it, so at LowWidth's default of 0% - where the low band's
// contribution is exactly zero regardless of what it contains - this
// reproduces the v0.1 behaviour of forcing the low band to silence exactly,
// a strict generalisation rather than a behaviour change. At any other
// LowWidth, or whenever both bands are re-summed with a nonzero gain, the
// result is NOT a null/identity operation even at LowWidth == Width == 100%:
// per JUCE's own documentation, a Linkwitz-Riley crossover's low+high sum is
// a flat-*magnitude* allpass, not the original signal (see the class-level
// note on the crossover itself, below) - this is the standard, expected
// characteristic of any Linkwitz-Riley-crossover-based multiband processor.
// With the crossover off (BassMonoFreq == 0), Width alone scales the entire
// Side signal as a single band, exactly as in v0.1.
//
// Auto Mono Safety: a running, leaky-integrated correlation estimate of the
// plugin's *input* L/R (computed every sample regardless of whether the
// safety feature is engaged, so it is always available to drive a future
// GUI meter - see getCorrelationValue()) is used, when enabled, to further
// attenuate the Side signal in proportion to how out-of-phase the input is.
// Because this only ever scales Side and never touches Mid, it preserves
// the exact same mono-sum invariant (L + R == 2 * Mid at any setting) that
// Width and multiband width already rely on - see docs/architecture.md. The
// on/off toggle itself is crossfaded (not an instant gate), since the
// correlation estimate keeps running while the feature is off and can
// already be sitting at its floor the instant it is engaged.
//
// v0.2.0 (docs/design-brief.md) research-driven revisions to Auto Mono
// Safety, all sourced from documented professional correlation-meter
// convention:
//   - Ballistics: correlationTimeConstantSeconds moved from 200ms to 300ms
//     (a reasoned compromise toward, not all the way to, the ~600ms
//     documented for a pure *display* meter - Auto Mono Safety is a live
//     control-loop input as well as a future meter feed).
//   - Dead-zone: correlation in [autoMonoSafetyDeadZone, 1.0] (-0.10 to 1.0)
//     now yields full (1.0) safety gain, not just correlation >= 0 - "the
//     occasional small deviation into the negative side is usually
//     insignificant" (documented correlation-meter practice). The ramp
//     toward the floor now runs from -0.10 down to -1.0.
//   - The floor gain is now user-exposed (autoMonoSafetyFloorDb, -24 to
//     0 dB, default -9.1 dB - reproduces the old hardcoded 0.35 linear floor
//     bit-for-bit at default) rather than a fixed internal constant.
//   - Multiband (autoMonoSafetyMultiband, off by default): when on and the
//     bass-mono crossover is engaged, the correlation-derived gain is
//     computed and applied independently for the low/high bands already
//     split out by that crossover (via a second pair of mono
//     LinkwitzRileyFilters run on the raw input L/R, always processing -
//     same "always process, conditionally use" pattern as the bass-mono
//     crossover itself), instead of one broadband estimate scaling both.
//     When off, or bass-mono is off, behaviour is unchanged (single
//     broadband estimate).
//
// Haas Mode: an alternative, non-M/S widening technique applied *after* M/S
// decode - the Right channel is delayed by HaasTimeMs relative to Left via a
// juce::dsp::DelayLine. Unlike Width-based widening, this does NOT preserve
// the exact mono-sum invariant (summing two channels that are offset in time
// is not equivalent to summing the original, undelayed pair) - it trades
// that guarantee for a different, well-known psychoacoustic widening effect
// (the precedence effect). It is off by default and orthogonal to
// Width/multiband width/Auto Mono Safety, which all operate purely in the
// M/S domain before Haas Mode's post-decode delay is applied.
//
// Decorrelate (v0.2.0, docs/design-brief.md): a second, gentler alternative
// widening technique for near-mono material, also applied post-M/S-decode to
// the Right channel, alongside Haas Mode. Where Haas Mode trades the exact
// mono-sum guarantee for a strong precedence-effect delay (comb-filtering on
// mono fold-down), Decorrelate trades a much smaller, documented cost (mild
// spectral ripple, typically 1-2 dB) via a cascade of allpass IIR filters
// (juce::dsp::IIR::Filter, spread across several fixed frequencies) blended
// with the dry Right signal by decorrelateAmount. Like the bass-mono
// crossover's dual-output processSample() and Haas Mode's delay line, the
// allpass cascade always processes every sample (even while disabled) so its
// internal state never goes stale; only the blend is conditional. Decorrelate
// and Haas Mode are mutually exclusive: whenever both are enabled,
// Decorrelate takes effect and the Haas delay line is pinned to 0 samples
// (an exact passthrough) for as long as Decorrelate stays engaged - both
// operate on the same post-decode Right channel for the same underlying
// goal, and stacking them would make each one's already-approximate
// mono-fold-down cost impossible to reason about independently. Decorrelate
// is presented in the manual as "gentler, more mono-tolerant", not
// "mono-safe" - it remains, like Haas Mode, an explicit, documented exception
// to the mono-sum invariant, just a smaller one; the Width/Low Width/Auto
// Mono Safety invariant itself (broadband or multiband) is completely
// unaffected by either, since both sit strictly after M/S decode. Like the
// bass-mono crossover and Haas Mode, the allpass cascade is a direct-form IIR
// structure - zero-latency, sample-synchronous - so it never adds to
// getLatencySamples().
//
// The bass-mono crossover itself is unchanged from v0.1: a single-channel
// juce::dsp::LinkwitzRileyFilter (JUCE 8.0.14, prepared mono, dual-output
// processSample). Per JUCE's own class documentation (juce_dsp/processors/
// juce_LinkwitzRileyFilter.h), "their sum is equivalent to an all-pass
// filter with a flat magnitude frequency response" - i.e. low+high preserves
// magnitude but NOT phase/identity (confirmed empirically during
// development: summing the unscaled bands reproduces the input's RMS level
// but not its sample values). The v0.1 bass-mono feature only ever
// *discards* the low band and keeps the high band alone, so this never
// mattered for it - a highpass filter's output is genuinely close to zero
// below its cutoff on its own, independent of any sum-identity claim. This
// filter structure introduces no additional latency regardless (a direct-
// form IIR, sample-synchronous by construction), so Firmament's total
// reported latency is still always 0 samples - Haas Mode's delay is a
// separate, intentional *relative* channel-to-channel offset (the whole
// point of that effect), not a processing artifact a host needs to
// compensate for, the same way a chorus/flanger's internal modulated delay
// is not reported as plugin latency either.
class FirmamentEngine
{
public:
    FirmamentEngine();

    // Allocates all DSP state. Must be called (and completed) before the
    // first process() call, and again whenever sample rate/block size/
    // channel count change. `spec.numChannels` is expected to be 2 (stereo);
    // process() is a safe no-op for any other channel count.
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears all filter/gain/delay-line state without deallocating. Safe to
    // call from the audio thread (e.g. on playback stop/loop).
    void reset();

    // Processes `block` in place. `block` must be a 2-channel (stereo)
    // block, channel 0 = left, channel 1 = right, with at most the maximum
    // sample count declared to prepare(); a zero-sample or non-stereo block
    // is a safe no-op. No allocation occurs here.
    void process (juce::dsp::AudioBlock<float>& block) noexcept;

    // Parameter setters, in real units. Safe to call every block from the
    // audio thread - no allocation/locks.
    void setWidthPercent (float newWidthPercent);
    void setLowWidthPercent (float newLowWidthPercent);
    void setBassMonoFrequencyHz (float newFrequencyHz);
    void setAutoMonoSafetyEnabled (bool shouldBeEnabled);
    void setHaasEnabled (bool shouldBeEnabled);
    void setHaasTimeMs (float newHaasTimeMs);
    void setOutputDb (float newOutputDb);

    // v0.2.0 additions - see the class-level comment above and
    // docs/design-brief.md for the full rationale of each.
    void setAutoMonoSafetyFloorDb (float newFloorDb);
    void setAutoMonoSafetyMultibandEnabled (bool shouldBeEnabled);
    void setDecorrelateEnabled (bool shouldBeEnabled);
    void setDecorrelateAmountPercent (float newAmountPercent);

    // The most recent block's running correlation estimate of the plugin's
    // *input* L/R signal, in [-1, 1] (1 = perfectly in-phase/mono-compatible,
    // 0 = uncorrelated, -1 = perfectly out-of-phase). Updated once per
    // process() call (not per-sample), safe to read from any thread - this
    // is what drives Auto Mono Safety internally and is exposed so a future
    // GUI (M3) can display it as a correlation/phase meter without any
    // further DSP work.
    float getCorrelationValue() const noexcept { return lastCorrelation; }

    // Firmament's own processing chain (Width/multiband/Auto Mono
    // Safety/bass-mono crossover/output trim) never adds latency; Haas
    // Mode's delay is an intentional relative Left/Right offset (the effect
    // itself), not a processing artifact, so it is likewise never reported -
    // see the class-level comment above. getLatencySamples() is therefore
    // always exactly 0, reported via a static constexpr.
    static constexpr int getLatencySamples() noexcept { return 0; }

private:
    static constexpr double smoothingTimeSeconds = 0.05;

    // Correlation meter ballistics: a one-pole leaky-integrator time constant
    // for Auto Mono Safety/the correlation meter. v0.2.0 (docs/design-brief.md)
    // moves this from 200ms to 300ms - a reasoned compromise toward, but not
    // all the way to, the ~600ms documented for a pure *display* correlation
    // meter (Auto Mono Safety is a live control-loop input as well as a
    // future meter feed, so staying meaningfully faster than a passive
    // display is deliberate).
    static constexpr double correlationTimeConstantSeconds = 0.3;

    // Auto Mono Safety dead-zone (v0.2.0): correlation in [-0.10, 1.0] yields
    // full (1.0) safety gain; the ramp toward the floor runs from -0.10 down
    // to -1.0, not from 0.0 - "the occasional small deviation into the
    // negative side is usually insignificant" (documented correlation-meter
    // practice, see docs/research-notes.md Section 5). -0.10 itself is a
    // reasoned magnitude (no source gives an exact number). Not user-exposed
    // - like the ballistics time constant below, this is an internal
    // constant defined alongside computeAutoMonoSafetyGain() in
    // FirmamentEngine.cpp, not declared here.
    //
    // Maximum Haas Mode delay the DelayLine is sized for; matches the
    // haasTimeMs parameter's documented maximum (see ParameterLayout.cpp)
    // with a small margin.
    static constexpr float maxHaasTimeMs = 41.0f;

    // Number of cascaded allpass stages in the Decorrelate network, and the
    // fixed frequencies (Hz) each stage's allpass coefficients are centred
    // on - spread across the spectrum so the resulting phase shift is
    // irregular rather than a single time offset (docs/research-notes.md
    // Section 4: "phase shifts spread irregularly across the spectrum"),
    // which is what keeps mono-fold-down cost to mild ripple rather than
    // deep comb-filter notches. The exact stage count/frequency spacing is
    // implementation-reasoned (docs/design-brief.md) - no source publishes
    // an exact allpass topology for this category of effect.
    static constexpr int numDecorrelateStages = 4;
    static constexpr std::array<float, numDecorrelateStages> decorrelateStageFrequenciesHz { 300.0f, 900.0f, 2700.0f, 8100.0f };
    static constexpr float decorrelateStageQ = 0.7f;

    double sampleRate = 44100.0;

    // Operates on a single channel (the one derived Side stream), not the
    // stereo bus - prepared with numChannels == 1 regardless of the block's
    // own channel count. Also used for the low/high multiband split (see
    // class-level comment).
    juce::dsp::LinkwitzRileyFilter<float> bassMonoCrossover;
    juce::dsp::Gain<float> outputGain;

    // v0.2.0 multiband Auto Mono Safety: a second pair of mono
    // LinkwitzRileyFilters, run on the raw (pre-Width) input Left/Right
    // channels at the same crossover frequency as bassMonoCrossover, so the
    // per-band correlation estimate below can be computed independently for
    // the low/high bands. Always processed (same "always process,
    // conditionally use" pattern as bassMonoCrossover - see GitHub issue
    // #12's fix) regardless of whether Multiband is actually engaged, so
    // their internal TPT state never goes stale.
    juce::dsp::LinkwitzRileyFilter<float> leftMultibandCrossover;
    juce::dsp::LinkwitzRileyFilter<float> rightMultibandCrossover;

    // Decorrelate's allpass cascade (Right channel only, mono) - see the
    // class-level comment above. Always processed regardless of whether
    // Decorrelate is enabled, so its state never goes stale when re-enabled.
    std::array<juce::dsp::IIR::Filter<float>, numDecorrelateStages> decorrelateAllpassStages;

    // Haas Mode delay line - Right channel only, Left is passed through
    // unchanged. Always pushed/popped (even while Haas Mode is off, with the
    // delay pinned to 0 samples) so enabling it mid-stream never starts from
    // stale/discontinuous history. Also pinned to 0 samples while Decorrelate
    // is engaged (mutual exclusivity - see the class-level comment above).
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> haasDelayLine;

    // Width is a plain multiplicative scale on the Side channel, cheap
    // enough to interpolate per-sample directly (no trig/coefficient
    // recompute involved), so it uses per-sample getNextValue() rather than
    // the once-per-block skip() pattern used for the crossover frequency
    // below. Low Width uses the same smoothing scheme.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowWidthSmoothed;

    // Frequency is smoothed multiplicatively (appropriate for a quantity
    // perceived logarithmically) but only ever driven while bass-mono is
    // enabled (lastBassMonoHz > 0); recomputing LinkwitzRileyFilter
    // coefficients involves a tan() call, so - like Overture's Tight/Tone
    // filters - this is re-derived once per block rather than per sample.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> bassMonoFrequencySmoothed;

    // Haas Mode delay time, smoothed and re-applied to the delay line once
    // per block (same cadence as the crossover frequency above) rather than
    // per sample - real-time-safe modulation of DelayLine::setDelay() is
    // cheap, but there is no audible benefit to sample-accurate updates for
    // a manually-set widening parameter.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> haasTimeMsSmoothed;

    // Auto Mono Safety's on/off toggle is crossfaded rather than applied as
    // an instant per-block gate (GitHub issue #13): the correlation-derived
    // safetyGain below is always computed (the correlation sums are already
    // running unconditionally), and this ramps between 0 (fully bypassed -
    // effective Side gain pinned to 1.0) and 1 (fully engaged - effective
    // Side gain equals safetyGain) over the same smoothingTimeSeconds used
    // elsewhere, so flipping the toggle while the correlation estimate is
    // already settled near its worst case (~-9 dB) can no longer step the
    // Side gain in a single sample.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> autoMonoSafetyAmountSmoothed;

    // v0.2.0: Auto Mono Safety's floor gain (linear), smoothed the same way
    // as Width/Low Width - a plain multiplicative endpoint of the dead-zone
    // ramp below, cheap enough to interpolate per-sample.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> autoMonoSafetyFloorGainSmoothed;

    // v0.2.0: Decorrelate's dry/wet blend amount (0..1), smoothed like
    // Width/Low Width. Always advances even while Decorrelate is off (same
    // pattern as haasTimeMsSmoothed), so re-enabling it starts from an
    // up-to-date value. decorrelateEnabled itself is an instant per-block
    // gate, like haasEnabled - not crossfaded like Auto Mono Safety's toggle,
    // since (unlike Auto Mono Safety's correlation-derived gain) there is no
    // hidden state that can already be sitting at an extreme the instant the
    // feature is engaged; the always-running allpass cascade plus this
    // smoothed amount are enough to avoid a discontinuity on toggle.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> decorrelateAmountSmoothed;

    // Running leaky-integrator sums driving the correlation estimate (see
    // getCorrelationValue()); kept in double for precision over long runs.
    // Computed from the *input* L/R every sample.
    double correlationSumLR = 0.0;
    double correlationSumLL = 0.0;
    double correlationSumRR = 0.0;
    double correlationSmoothingCoeff = 0.0;
    float lastCorrelation = 0.0f;

    // v0.2.0 multiband Auto Mono Safety: identical leaky-integrator sums as
    // above, computed independently for the low/high bands split out by
    // leftMultibandCrossover/rightMultibandCrossover. Always running (same
    // "always process" rationale as the crossovers themselves).
    double correlationSumLRLow = 0.0;
    double correlationSumLLLow = 0.0;
    double correlationSumRRLow = 0.0;
    double correlationSumLRHigh = 0.0;
    double correlationSumLLHigh = 0.0;
    double correlationSumRRHigh = 0.0;

    // Last commanded values, re-applied to the smoothers on every prepare()
    // so re-preparing (sample-rate change, etc.) never resets a live
    // parameter back to a default or lets a smoother start from an invalid
    // value. 0 Hz is the frozen "bass-mono off" sentinel (see ParameterIds.h)
    // and is deliberately never fed to the smoother/filter, which requires a
    // strictly positive, sub-Nyquist cutoff.
    float lastWidthPercent = 100.0f;
    float lastLowWidthPercent = 0.0f;
    float lastBassMonoHz = 0.0f;
    bool lastAutoMonoSafetyEnabled = false;
    bool lastHaasEnabled = false;
    float lastHaasTimeMs = 20.0f;

    // v0.2.0 additions - defaults reproduce v0.1.1 behaviour exactly.
    float lastAutoMonoSafetyFloorDb = -9.1f;
    bool lastAutoMonoSafetyMultibandEnabled = false;
    bool lastDecorrelateEnabled = false;
    float lastDecorrelateAmountPercent = 50.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FirmamentEngine)
};
