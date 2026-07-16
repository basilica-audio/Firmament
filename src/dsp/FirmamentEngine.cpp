#include "FirmamentEngine.h"

#include <cmath>

namespace
{
    // Keeps a requested bass-mono crossover frequency strictly below Nyquist
    // and strictly positive, as juce::dsp::LinkwitzRileyFilter::
    // setCutoffFrequency() requires (JUCE 8.0.14,
    // juce_dsp/processors/juce_LinkwitzRileyFilter.cpp asserts
    // isPositiveAndBelow(cutoff, sampleRate * 0.5)). 500 Hz (the parameter's
    // documented maximum) is far below Nyquist at any realistic audio sample
    // rate, but this guard costs nothing and removes the assumption entirely.
    float clampBelowNyquist (float frequencyHz, double sampleRate) noexcept
    {
        const auto nyquist = static_cast<float> (sampleRate) * 0.5f;
        const auto maxHz = juce::jmax (5.0f, nyquist * 0.9f);
        return juce::jlimit (5.0f, maxHz, frequencyHz);
    }

    // Small floor added under the correlation denominator so a silent (or
    // near-silent) input never produces a divide-by-zero/NaN correlation
    // estimate - with both sums at (near) zero this yields a well-defined
    // ~0 (uncorrelated) rather than NaN.
    constexpr double correlationEpsilon = 1.0e-12;

    // Auto Mono Safety's minimum Side gain, applied when the running
    // correlation estimate reaches fully out-of-phase (-1.0). Chosen to
    // audibly rein in a collapsing mono sum without ever fully muting Side
    // (which would be a much more drastic, surprising behaviour than a
    // "safety" feature should have).
    constexpr float autoMonoSafetyFloorGain = 0.35f;
}

FirmamentEngine::FirmamentEngine() = default;

void FirmamentEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    // The bass-mono crossover runs on a single derived stream (the Side
    // channel), independent of however many channels the host bus has.
    juce::dsp::ProcessSpec monoSpec = spec;
    monoSpec.numChannels = 1;
    bassMonoCrossover.prepare (monoSpec);

    outputGain.setRampDurationSeconds (smoothingTimeSeconds);
    outputGain.prepare (spec);

    // Haas Mode's delay line likewise only ever touches one derived stream
    // (the decoded Right channel), so it is prepared mono too. The maximum
    // delay is sized from maxHaasTimeMs with a small integer-sample margin;
    // setMaximumDelayInSamples() reallocates, so this only ever runs from
    // prepare() (message thread), never from process().
    haasDelayLine.prepare (monoSpec);
    const auto maxHaasSamples = static_cast<int> (std::ceil ((maxHaasTimeMs / 1000.0) * sampleRate)) + 4;
    haasDelayLine.setMaximumDelayInSamples (maxHaasSamples);

    widthSmoothed.reset (sampleRate, smoothingTimeSeconds);
    widthSmoothed.setCurrentAndTargetValue (lastWidthPercent * 0.01f);

    lowWidthSmoothed.reset (sampleRate, smoothingTimeSeconds);
    lowWidthSmoothed.setCurrentAndTargetValue (lastLowWidthPercent * 0.01f);

    haasTimeMsSmoothed.reset (sampleRate, smoothingTimeSeconds);
    haasTimeMsSmoothed.setCurrentAndTargetValue (lastHaasTimeMs);

    autoMonoSafetyAmountSmoothed.reset (sampleRate, smoothingTimeSeconds);
    autoMonoSafetyAmountSmoothed.setCurrentAndTargetValue (lastAutoMonoSafetyEnabled ? 1.0f : 0.0f);

    // Seed the frequency smoother with a safe, strictly-positive value even
    // while bass-mono is off, so that if it is engaged later mid-stream the
    // smoother starts from a sane point rather than 0 Hz (an invalid filter
    // cutoff). This has no audible effect while disabled, since process()
    // never calls setCutoffFrequency()/runs the crossover unless
    // lastBassMonoHz > 0.
    const auto seedFrequencyHz = lastBassMonoHz > 0.0f ? juce::jmax (5.0f, lastBassMonoHz) : 20.0f;
    bassMonoFrequencySmoothed.reset (sampleRate, smoothingTimeSeconds);
    bassMonoFrequencySmoothed.setCurrentAndTargetValue (clampBelowNyquist (seedFrequencyHz, sampleRate));

    // 200 ms one-pole leaky-integrator coefficient for the correlation
    // meter/Auto Mono Safety estimate - see the class-level comment in
    // FirmamentEngine.h.
    correlationSmoothingCoeff = sampleRate > 0.0
                                     ? std::exp (-1.0 / (sampleRate * correlationTimeConstantSeconds))
                                     : 0.0;

    reset();

    // Prime the filter coefficients immediately so the very first process()
    // call runs with correct, non-default coefficients rather than an
    // identity/uninitialised state, in case bass-mono is already engaged
    // when prepare() runs.
    bassMonoCrossover.setCutoffFrequency (clampBelowNyquist (seedFrequencyHz, sampleRate));
}

void FirmamentEngine::reset()
{
    bassMonoCrossover.reset();
    outputGain.reset();
    haasDelayLine.reset();

    correlationSumLR = 0.0;
    correlationSumLL = 0.0;
    correlationSumRR = 0.0;
    lastCorrelation = 0.0f;
}

void FirmamentEngine::setWidthPercent (float newWidthPercent)
{
    lastWidthPercent = newWidthPercent;
    widthSmoothed.setTargetValue (newWidthPercent * 0.01f);
}

void FirmamentEngine::setLowWidthPercent (float newLowWidthPercent)
{
    lastLowWidthPercent = newLowWidthPercent;
    lowWidthSmoothed.setTargetValue (newLowWidthPercent * 0.01f);
}

void FirmamentEngine::setBassMonoFrequencyHz (float newFrequencyHz)
{
    lastBassMonoHz = newFrequencyHz;

    // 0 Hz (off) is deliberately never forwarded to the smoother/filter -
    // process() gates the whole crossover stage on lastBassMonoHz > 0.
    if (newFrequencyHz > 0.0f)
        bassMonoFrequencySmoothed.setTargetValue (juce::jmax (5.0f, newFrequencyHz));
}

void FirmamentEngine::setAutoMonoSafetyEnabled (bool shouldBeEnabled)
{
    lastAutoMonoSafetyEnabled = shouldBeEnabled;
    autoMonoSafetyAmountSmoothed.setTargetValue (shouldBeEnabled ? 1.0f : 0.0f);
}

void FirmamentEngine::setHaasEnabled (bool shouldBeEnabled)
{
    lastHaasEnabled = shouldBeEnabled;
}

void FirmamentEngine::setHaasTimeMs (float newHaasTimeMs)
{
    lastHaasTimeMs = newHaasTimeMs;
    haasTimeMsSmoothed.setTargetValue (newHaasTimeMs);
}

void FirmamentEngine::setOutputDb (float newOutputDb)
{
    outputGain.setGainDecibels (newOutputDb);
}

void FirmamentEngine::process (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto numSamples = block.getNumSamples();

    if (numSamples == 0)
        return;

    // The M/S encode/decode below is only meaningful for a genuine stereo
    // pair; the processor guarantees this (duplicating mono input to both
    // channels before calling process(), see PluginProcessor.cpp), but this
    // guard makes the engine safe to call standalone (e.g. from a test) with
    // any other channel count too.
    if (block.getNumChannels() != 2)
        return;

    const bool bassMonoEnabled = lastBassMonoHz > 0.0f;
    const bool haasEnabled = lastHaasEnabled;

    // Coefficient recomputation involves a tan() call, so - like Overture's
    // Tight/Tone filters - the crossover frequency is smoothed and re-derived
    // once per block rather than per sample. This now runs unconditionally
    // (not just while bassMonoEnabled) so the crossover's coefficients - and,
    // in the per-sample loop below, its internal TPT state (s1-s4) - keep
    // tracking the live Side signal even while the section is disabled (the
    // same "always process, conditionally use" pattern the Haas delay line
    // below already follows). This fixes GitHub issue #12: previously s1-s4
    // were simply never touched while disabled, so re-engaging (e.g.
    // BassMonoFreq automation sweeping back up through 0 Hz) resumed
    // filtering from a stale snapshot instead of live state, producing an
    // audible transient. The frequency smoother's target only ever moves
    // while bass-mono is actually commanded on (see
    // setBassMonoFrequencyHz()), so while disabled this simply holds/settles
    // at the last commanded (or prepare()-seeded) frequency rather than
    // drifting - exactly matching what a continuously-engaged crossover
    // would do once re-enabled at the same frequency.
    const auto freqHz = clampBelowNyquist (bassMonoFrequencySmoothed.skip (static_cast<int> (numSamples)), sampleRate);
    bassMonoCrossover.setCutoffFrequency (freqHz);

    // Haas Mode's delay time is likewise re-derived once per block rather
    // than per sample (there is no audible benefit to sample-accurate
    // updates for a manually-set widening parameter, and DelayLine::
    // setDelay() is cheap regardless). The smoother always advances, even
    // while Haas Mode is off, so re-enabling it later starts from an
    // up-to-date value instead of an initial 20 ms default; the delay line
    // itself is pinned to 0 samples while disabled (see the per-sample loop
    // below), so this has no audible effect until Haas Mode is engaged.
    const auto haasMs = haasTimeMsSmoothed.skip (static_cast<int> (numSamples));

    if (haasEnabled)
    {
        const auto maxDelaySamples = static_cast<float> (haasDelayLine.getMaximumDelayInSamples());
        const auto delaySamples = juce::jlimit (0.0f, maxDelaySamples, haasMs * 0.001f * static_cast<float> (sampleRate));
        haasDelayLine.setDelay (delaySamples);
    }
    else
    {
        haasDelayLine.setDelay (0.0f);
    }

    auto* left = block.getChannelPointer (0);
    auto* right = block.getChannelPointer (1);

    for (size_t i = 0; i < numSamples; ++i)
    {
        // Width/Low Width are plain multiplicative scales with no
        // coefficients to recompute, so they are cheap enough to interpolate
        // sample-accurately; Low Width is only audible while the bass-mono
        // crossover is engaged (see below) but is always advanced so it is
        // caught up with its target the instant the crossover is enabled.
        const auto widthProportion = widthSmoothed.getNextValue();
        const auto lowWidthProportion = lowWidthSmoothed.getNextValue();

        // A single NaN/Inf input sample must never be allowed to reach the
        // bass-mono crossover: unlike a feedforward gain stage, an IIR
        // filter's internal state (s1-s4) carries a poisoned value forward
        // indefinitely once it gets in (NaN propagates through any further
        // multiply/add), permanently corrupting every subsequent block even
        // once clean input resumes. Scrubbing here - cheap, branch-only, no
        // allocation - keeps that failure mode local to the single bad
        // sample instead of the rest of the stream.
        const auto leftSample = std::isfinite (left[i]) ? left[i] : 0.0f;
        const auto rightSample = std::isfinite (right[i]) ? right[i] : 0.0f;

        // Correlation meter / Auto Mono Safety: a leaky-integrated running
        // estimate of the plugin's *input* L/R correlation, updated every
        // sample regardless of whether Auto Mono Safety is enabled, so it is
        // always current for getCorrelationValue() (a future GUI meter).
        // Deriving it from the raw scrubbed input (rather than the Width-
        // scaled output) keeps it a direct read of the source material's
        // own mono-compatibility risk and avoids any feedback loop with the
        // Side scaling computed from it below.
        const auto coeff = correlationSmoothingCoeff;
        correlationSumLR = coeff * correlationSumLR + (1.0 - coeff) * (static_cast<double> (leftSample) * static_cast<double> (rightSample));
        correlationSumLL = coeff * correlationSumLL + (1.0 - coeff) * (static_cast<double> (leftSample) * static_cast<double> (leftSample));
        correlationSumRR = coeff * correlationSumRR + (1.0 - coeff) * (static_cast<double> (rightSample) * static_cast<double> (rightSample));

        const auto encoded = MidSideCodec::encode (leftSample, rightSample);

        // The crossover is always run against the live (unscaled) Side
        // signal - even while bass-mono is disabled - so its state stays
        // synced with the live input; only the low/high split output is
        // conditionally used below (see the block-level comment above on
        // GitHub issue #12).
        float lowBand = 0.0f;
        float highBand = 0.0f;
        bassMonoCrossover.processSample (0, encoded.side, lowBand, highBand);

        float side;

        if (bassMonoEnabled)
        {
            // Splitting the *unscaled* Side signal into low/high bands and
            // scaling each independently before summing commutes exactly
            // with scale-then-filter (both are linear operations), so at the
            // default Low Width of 0% - where the low band contributes
            // nothing regardless of its content - this reproduces the v0.1
            // "bass mono forces the low band to silence" behaviour precisely.
            // At any other Low Width the recombined result is NOT a null/
            // identity operation even when Low Width == Width - see the
            // class-level comment in FirmamentEngine.h for why (the
            // crossover's low+high sum is a flat-magnitude allpass, not the
            // original signal, per JUCE's own documentation).
            side = lowBand * lowWidthProportion + highBand * widthProportion;
        }
        else
        {
            // Single-band mode (crossover off): Width alone scales the whole
            // Side signal, exactly as in v0.1.
            side = encoded.side * widthProportion;
        }

        // Auto Mono Safety: the correlation-derived safety gain is always
        // computed (the correlation sums above are already running
        // unconditionally), and blended between full bypass (1.0) and fully
        // engaged (safetyGain) via a smoothed 0..1 amount rather than gated
        // as an instant per-block step (GitHub issue #13) - this avoids the
        // toggle itself stepping the Side gain by up to ~9 dB in a single
        // sample when the correlation estimate is already settled near its
        // floor at the moment the feature is switched on. While permanently
        // off, autoMonoAmount is pinned at 0 and effectiveSafetyGain is
        // exactly 1.0, so this is a bit-exact no-op passthrough, matching
        // the previous behaviour.
        const auto denominator = std::sqrt (correlationSumLL * correlationSumRR + correlationEpsilon);
        const auto correlationEstimate = static_cast<float> (juce::jlimit (-1.0, 1.0, correlationSumLR / denominator));

        // Full Side gain while the input is in-phase or uncorrelated
        // (correlation >= 0); linearly rein it in toward
        // autoMonoSafetyFloorGain as correlation approaches -1 (fully
        // out-of-phase) - a safety net against a widened signal collapsing
        // destructively on mono fold-down. This only ever scales Side, so -
        // like Width/Low Width - it never touches Mid and cannot break the
        // L + R == 2 * Mid mono-sum invariant.
        const auto safetyGain = correlationEstimate >= 0.0f
                                     ? 1.0f
                                     : juce::jmap (correlationEstimate, -1.0f, 0.0f, autoMonoSafetyFloorGain, 1.0f);

        const auto autoMonoAmount = autoMonoSafetyAmountSmoothed.getNextValue();
        const auto effectiveSafetyGain = 1.0f + autoMonoAmount * (safetyGain - 1.0f);
        side *= effectiveSafetyGain;

        const auto decoded = MidSideCodec::decode (encoded.mid, side);

        left[i] = decoded.left;

        // Haas Mode: the delay line is always pushed/popped, even while
        // disabled (delay pinned to 0 samples above), so it is a bit-exact
        // passthrough when off and re-enabling it mid-stream never starts
        // from stale/discontinuous buffered history.
        haasDelayLine.pushSample (0, decoded.right);
        right[i] = haasDelayLine.popSample (0);
    }

    lastCorrelation = static_cast<float> (juce::jlimit (-1.0, 1.0, correlationSumLR / std::sqrt (correlationSumLL * correlationSumRR + correlationEpsilon)));

    juce::dsp::ProcessContextReplacing<float> context (block);
    outputGain.process (context);
}
