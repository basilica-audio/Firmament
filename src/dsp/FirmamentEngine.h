#pragma once

#include <juce_dsp/juce_dsp.h>

#include "MidSideCodec.h"

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

    // Correlation meter ballistics: a 200 ms one-pole leaky-integrator time
    // constant, typical for phase/correlation meters (fast enough to react
    // usefully within Auto Mono Safety's control loop, slow enough not to
    // flicker sample-to-sample).
    static constexpr double correlationTimeConstantSeconds = 0.2;

    // Maximum Haas Mode delay the DelayLine is sized for; matches the
    // haasTimeMs parameter's documented maximum (see ParameterLayout.cpp)
    // with a small margin.
    static constexpr float maxHaasTimeMs = 41.0f;

    double sampleRate = 44100.0;

    // Operates on a single channel (the one derived Side stream), not the
    // stereo bus - prepared with numChannels == 1 regardless of the block's
    // own channel count. Also used for the low/high multiband split (see
    // class-level comment).
    juce::dsp::LinkwitzRileyFilter<float> bassMonoCrossover;
    juce::dsp::Gain<float> outputGain;

    // Haas Mode delay line - Right channel only, Left is passed through
    // unchanged. Always pushed/popped (even while Haas Mode is off, with the
    // delay pinned to 0 samples) so enabling it mid-stream never starts from
    // stale/discontinuous history.
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

    // Running leaky-integrator sums driving the correlation estimate (see
    // getCorrelationValue()); kept in double for precision over long runs.
    // Computed from the *input* L/R every sample.
    double correlationSumLR = 0.0;
    double correlationSumLL = 0.0;
    double correlationSumRR = 0.0;
    double correlationSmoothingCoeff = 0.0;
    float lastCorrelation = 0.0f;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FirmamentEngine)
};
