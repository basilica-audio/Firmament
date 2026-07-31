#include "FirmamentEngine.h"

#include <cmath>

namespace
{
    // Keeps a requested crossover frequency strictly below Nyquist and
    // strictly positive, as juce::dsp::LinkwitzRileyFilter::
    // setCutoffFrequency() requires (JUCE 8.0.14,
    // juce_dsp/processors/juce_LinkwitzRileyFilter.cpp asserts
    // isPositiveAndBelow(cutoff, sampleRate * 0.5)).
    float clampBelowNyquist (float frequencyHz, double sampleRate) noexcept
    {
        const auto nyquist = static_cast<float> (sampleRate) * 0.5f;
        const auto maxHz = juce::jmax (5.0f, nyquist * 0.9f);
        return juce::jlimit (5.0f, maxHz, frequencyHz);
    }

    // Small floor added under a correlation denominator so a silent (or
    // near-silent) input never produces a divide-by-zero/NaN correlation
    // estimate.
    constexpr double correlationEpsilon = 1.0e-12;

    // v0.3.0 energy gate (binding for every correlation estimator - display
    // meters and guard detectors alike, brief 3.4/3.8): when the product of
    // the two channel-energy sums falls below this gate, the gated estimate
    // decays toward 0 instead of holding its last ratio. A leaky Pearson
    // ratio is invariant under silence (numerator and denominator decay at
    // the same rate), so without this rule the guard would hold rho = -1
    // after an anti-phase burst into silence and never release, and the
    // display would show +/-1 on silence (beis.de rule). The gate value
    // corresponds to both channels sitting around -60 dBFS RMS.
    constexpr double correlationEnergyGate = 1.0e-12;

    // Auto Mono Safety's dead-zone/floor mapping (v0.2.0, unchanged):
    // correlation in [deadZone, 1.0] yields full (1.0) gain; below the
    // dead-zone, gain ramps linearly down to `floorGain` as correlation
    // approaches -1.0. Shared by the Smooth (300 ms static) and Dynamic
    // (30 ms detector + ballistics) safety paths - only the estimator and
    // the gain smoothing differ between the modes, never the map.
    constexpr float autoMonoSafetyDeadZone = -0.10f;

    float computeAutoMonoSafetyGain (float correlationEstimate, float floorGain) noexcept
    {
        if (correlationEstimate >= autoMonoSafetyDeadZone)
            return 1.0f;

        return juce::jmap (correlationEstimate, -1.0f, autoMonoSafetyDeadZone, floorGain, 1.0f);
    }

    // One-pole leaky-integrator update shared by every correlation sum.
    inline void leakyUpdate (double& sum, double coeff, double product) noexcept
    {
        sum = coeff * sum + (1.0 - coeff) * product;
    }

    // Energy-gated Pearson estimate (see correlationEnergyGate above): with
    // signal present this computes exactly the v0.2.0 expression
    // jlimit(-1, 1, sumLR / sqrt(sumLL * sumRR + eps)) - bit-for-bit - and
    // only under the gate does it decay the held state toward 0 instead.
    inline float updateGatedCorrelation (double sumLR, double sumLL, double sumRR,
                                         float& gatedState, double decayCoeff) noexcept
    {
        const auto energyProduct = sumLL * sumRR;

        if (energyProduct < correlationEnergyGate)
        {
            gatedState = static_cast<float> (decayCoeff * static_cast<double> (gatedState));
            return gatedState;
        }

        const auto denominator = std::sqrt (energyProduct + correlationEpsilon);
        gatedState = static_cast<float> (juce::jlimit (-1.0, 1.0, sumLR / denominator));
        return gatedState;
    }

    // Crossfade with exact endpoint selection: settled weights of exactly 0
    // or 1 return the dry/wet operand *bit-exactly* (no "dry + 1*(wet-dry)"
    // rounding), which is what keeps every neutral-default path
    // bit-identical to v0.2.0 (brief 6.1a) while transitions stay
    // click-free.
    inline float blendSelect (float dry, float wet, float weight) noexcept
    {
        if (weight <= 0.0f)
            return dry;

        if (weight >= 1.0f)
            return wet;

        return dry + weight * (wet - dry);
    }

    // Dedicated asymmetric one-pole for the Dynamic safety gain (brief 3.4:
    // NOT a juce::SmoothedValue). Attack = gain falling (more attenuation),
    // release = gain recovering toward 1.
    inline void updateDynamicGain (float& gain, float target, double attackCoeff, double releaseCoeff) noexcept
    {
        const auto coeff = target < gain ? attackCoeff : releaseCoeff;
        gain += static_cast<float> (coeff * static_cast<double> (target - gain));
    }
}

FirmamentEngine::FirmamentEngine() = default;

void FirmamentEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    preparedMaxBlockSize = static_cast<int> (spec.maximumBlockSize);

    // Every crossover/allpass operates on a single derived stream,
    // independent of however many channels the host bus has.
    juce::dsp::ProcessSpec monoSpec = spec;
    monoSpec.numChannels = 1;
    bassMonoCrossover.prepare (monoSpec);
    sideHighSplitCrossover.prepare (monoSpec);

    leftMultibandCrossover.prepare (monoSpec);
    rightMultibandCrossover.prepare (monoSpec);
    leftHighSplitInputCrossover.prepare (monoSpec);
    rightHighSplitInputCrossover.prepare (monoSpec);

    midBassAllpass.prepare (monoSpec);
    midHighAllpass.prepare (monoSpec);
    lowBandHighAllpass.prepare (monoSpec);

    // Classic Decorrelate allpass cascade (v0.2.0, unchanged): coefficients
    // depend on sampleRate; makeAllPass() allocates, so this only ever runs
    // from prepare() (message thread), never from process().
    for (size_t stage = 0; stage < decorrelateAllpassStages.size(); ++stage)
    {
        decorrelateAllpassStages[stage].prepare (monoSpec);
        decorrelateAllpassStages[stage].coefficients =
            juce::dsp::IIR::Coefficients<float>::makeAllPass (sampleRate, decorrelateStageFrequenciesHz[stage], decorrelateStageQ);
    }

    // v0.3.0 velvet-noise pairs: tap tables rescaled to the current rate and
    // renormalised to unit energy (see VelvetDecorrelator.h).
    velvetDenseLeft.prepare (sampleRate, VelvetDecorrelator::Variant::dense30A);
    velvetDenseRight.prepare (sampleRate, VelvetDecorrelator::Variant::dense30B);
    velvetSparseLeft.prepare (sampleRate, VelvetDecorrelator::Variant::sparse15A);
    velvetSparseRight.prepare (sampleRate, VelvetDecorrelator::Variant::sparse15B);

    outputGain.setRampDurationSeconds (smoothingTimeSeconds);
    outputGain.prepare (spec);

    // Haas Mode delay line (Right channel only, prepared mono). v0.3.0:
    // Lagrange 3rd-order interpolation needs a small extra margin beyond the
    // maximum delay for its 4-tap kernel.
    haasDelayLine.prepare (monoSpec);
    const auto maxHaasSamples = static_cast<int> (std::ceil ((maxHaasTimeMs / 1000.0) * sampleRate)) + 8;
    haasDelayLine.setMaximumDelayInSamples (maxHaasSamples);

    widthSmoothed.reset (sampleRate, smoothingTimeSeconds);
    widthSmoothed.setCurrentAndTargetValue (lastWidthPercent * 0.01f);

    lowWidthSmoothed.reset (sampleRate, smoothingTimeSeconds);
    lowWidthSmoothed.setCurrentAndTargetValue (lastLowWidthPercent * 0.01f);

    highWidthSmoothed.reset (sampleRate, smoothingTimeSeconds);
    highWidthSmoothed.setCurrentAndTargetValue (lastHighWidthPercent * 0.01f);

    haasTimeMsSmoothed.reset (sampleRate, smoothingTimeSeconds);
    haasTimeMsSmoothed.setCurrentAndTargetValue (lastHaasTimeMs);

    autoMonoSafetyAmountSmoothed.reset (sampleRate, smoothingTimeSeconds);
    autoMonoSafetyAmountSmoothed.setCurrentAndTargetValue (lastAutoMonoSafetyEnabled ? 1.0f : 0.0f);

    autoMonoSafetyFloorGainSmoothed.reset (sampleRate, smoothingTimeSeconds);
    autoMonoSafetyFloorGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (lastAutoMonoSafetyFloorDb));

    decorrelateAmountSmoothed.reset (sampleRate, smoothingTimeSeconds);
    decorrelateAmountSmoothed.setCurrentAndTargetValue (lastDecorrelateAmountPercent * 0.01f);

    // Seed the frequency smoothers with safe, strictly-positive values even
    // while their stages are off, so engaging them later mid-stream starts
    // the smoothers from sane points rather than the 0 Hz "off" sentinel.
    const auto seedFrequencyHz = lastBassMonoHz > 0.0f ? juce::jmax (5.0f, lastBassMonoHz) : 20.0f;
    bassMonoFrequencySmoothed.reset (sampleRate, smoothingTimeSeconds);
    bassMonoFrequencySmoothed.setCurrentAndTargetValue (clampBelowNyquist (seedFrequencyHz, sampleRate));

    const auto seedHighSplitHz = lastHighSplitHz > 0.0f ? juce::jmax (5.0f, lastHighSplitHz) : 2000.0f;
    highSplitFrequencySmoothed.reset (sampleRate, smoothingTimeSeconds);
    highSplitFrequencySmoothed.setCurrentAndTargetValue (clampBelowNyquist (seedHighSplitHz, sampleRate));

    // v0.3.0 crossfade weights: reset and settle at their commanded targets
    // so a freshly prepared engine is already in its steady state.
    for (auto* smoother : { &decorrelateEnableSmoothed, &decorrelateClassicWeightSmoothed,
                            &decorrelateDenseWeightSmoothed, &decorrelateSparseWeightSmoothed,
                            &haasEnableSmoothed, &haasPinSmoothed,
                            &midBassApWeightSmoothed, &midHighApWeightSmoothed,
                            &highSplitBlendSmoothed, &safetyModeBlendSmoothed,
                            &widthCompGainSmoothed, &monoAuditionSmoothed })
        smoother->reset (sampleRate, smoothingTimeSeconds);

    linearPhaseFadeSmoothed.reset (sampleRate, linearPhaseFadeSeconds);

    refreshCrossfadeTargets();

    for (auto* smoother : { &decorrelateEnableSmoothed, &decorrelateClassicWeightSmoothed,
                            &decorrelateDenseWeightSmoothed, &decorrelateSparseWeightSmoothed,
                            &haasEnableSmoothed, &haasPinSmoothed,
                            &midBassApWeightSmoothed, &midHighApWeightSmoothed,
                            &highSplitBlendSmoothed, &safetyModeBlendSmoothed,
                            &widthCompGainSmoothed, &monoAuditionSmoothed })
        smoother->setCurrentAndTargetValue (smoother->getTargetValue());

    // Linear Phase engages directly (no mid-stream fade) when prepared with
    // it already commanded; the fade only handles live mode changes.
    linearPhaseActive = (lastBassMonoMode == static_cast<int> (BassMonoMode::linearPhase)) && lastBassMonoHz > 0.0f;
    linearPhaseFadeSmoothed.setCurrentAndTargetValue (1.0f);

    // Correlation ballistics coefficients (300 ms display/Smooth, 30 ms
    // fast/Dynamic) and the Dynamic gain smoother's asymmetric one-poles.
    correlationSmoothingCoeff = sampleRate > 0.0
                                     ? std::exp (-1.0 / (sampleRate * correlationTimeConstantSeconds))
                                     : 0.0;
    fastCorrelationSmoothingCoeff = sampleRate > 0.0
                                        ? std::exp (-1.0 / (sampleRate * fastCorrelationTimeConstantSeconds))
                                        : 0.0;
    safetyAttackCoeff = sampleRate > 0.0 ? 1.0 - std::exp (-1.0 / (sampleRate * safetyAttackSeconds)) : 1.0;
    safetyReleaseCoeff = sampleRate > 0.0 ? 1.0 - std::exp (-1.0 / (sampleRate * safetyReleaseSeconds)) : 1.0;

    // v0.3.0 Linear Phase tier: allocates its FIR/delay/convolution state
    // and synchronously computes + hands off the initial kernel (message
    // thread - prepare() is the documented synchronous recompute site).
    lpSideIn.assign (static_cast<size_t> (juce::jmax (1, preparedMaxBlockSize)), 0.0f);
    lpMidIn = lpSideIn;
    lpSideLow = lpSideIn;
    lpSideHigh = lpSideIn;
    lpMidDelayed = lpSideIn;
    linearPhase.prepare (monoSpec, clampBelowNyquist (seedFrequencyHz, sampleRate));

    reset();

    // Prime the filter coefficients immediately so the very first process()
    // call runs with correct, non-default coefficients.
    const auto primedFreqHz = clampBelowNyquist (seedFrequencyHz, sampleRate);
    bassMonoCrossover.setCutoffFrequency (primedFreqHz);
    leftMultibandCrossover.setCutoffFrequency (primedFreqHz);
    rightMultibandCrossover.setCutoffFrequency (primedFreqHz);
    midBassAllpass.setCutoffFrequency (primedFreqHz);

    const auto primedHighSplitHz = clampBelowNyquist (juce::jmax (seedHighSplitHz, 2.0f * primedFreqHz), sampleRate);
    sideHighSplitCrossover.setCutoffFrequency (primedHighSplitHz);
    leftHighSplitInputCrossover.setCutoffFrequency (primedHighSplitHz);
    rightHighSplitInputCrossover.setCutoffFrequency (primedHighSplitHz);
    midHighAllpass.setCutoffFrequency (primedHighSplitHz);
    lowBandHighAllpass.setCutoffFrequency (primedHighSplitHz);
}

void FirmamentEngine::reset()
{
    bassMonoCrossover.reset();
    sideHighSplitCrossover.reset();
    leftMultibandCrossover.reset();
    rightMultibandCrossover.reset();
    leftHighSplitInputCrossover.reset();
    rightHighSplitInputCrossover.reset();
    midBassAllpass.reset();
    midHighAllpass.reset();
    lowBandHighAllpass.reset();
    outputGain.reset();
    haasDelayLine.reset();
    linearPhase.reset();

    for (auto& stage : decorrelateAllpassStages)
        stage.reset();

    velvetDenseLeft.reset();
    velvetDenseRight.reset();
    velvetSparseLeft.reset();
    velvetSparseRight.reset();

    correlationSumLR = correlationSumLL = correlationSumRR = 0.0;
    correlationSumLRLow = correlationSumLLLow = correlationSumRRLow = 0.0;
    correlationSumLRHigh = correlationSumLLHigh = correlationSumRRHigh = 0.0;
    correlationSumLRMid = correlationSumLLMid = correlationSumRRMid = 0.0;
    correlationSumLRTop = correlationSumLLTop = correlationSumRRTop = 0.0;

    fastSumLR = fastSumLL = fastSumRR = 0.0;
    fastSumLRLow = fastSumLLLow = fastSumRRLow = 0.0;
    fastSumLRHigh = fastSumLLHigh = fastSumRRHigh = 0.0;
    fastSumLRMid = fastSumLLMid = fastSumRRMid = 0.0;
    fastSumLRTop = fastSumLLTop = fastSumRRTop = 0.0;

    outputSumLR = outputSumLL = outputSumRR = 0.0;

    gatedCorrelation = gatedCorrelationLow = gatedCorrelationHigh = 0.0f;
    gatedCorrelationMid = gatedCorrelationTop = 0.0f;
    gatedFastCorrelation = gatedFastCorrelationLow = gatedFastCorrelationHigh = 0.0f;
    gatedFastCorrelationMid = gatedFastCorrelationTop = 0.0f;
    gatedOutputCorrelation = 0.0f;

    dynamicGainBroadband = dynamicGainLow = dynamicGainHigh = 1.0f;
    dynamicGainMid = dynamicGainTop = 1.0f;

    lastCorrelation = 0.0f;
    lastCorrelationLow = lastCorrelationHigh = 0.0f;
    lastCorrelationMidBand = lastCorrelationHighBand = 0.0f;
    lastOutputCorrelation = 0.0f;
}

// ---------------------------------------------------------------------------
// Setters. Every v0.3.0 crossfade target depends on a small set of commanded
// values, so each setter simply records its value and re-derives all targets
// (cheap: a handful of comparisons and setTargetValue() calls, no
// allocation).

void FirmamentEngine::refreshCrossfadeTargets()
{
    const bool bassMonoEnabled = lastBassMonoHz > 0.0f;
    const bool highSplitEnabled = lastHighSplitHz > 0.0f;
    const bool phaseMatched = lastBassMonoMode == static_cast<int> (BassMonoMode::phaseMatched);

    decorrelateEnableSmoothed.setTargetValue (lastDecorrelateEnabled ? 1.0f : 0.0f);
    decorrelateClassicWeightSmoothed.setTargetValue (lastDecorrelateMode == static_cast<int> (DecorrelateMode::classic) ? 1.0f : 0.0f);
    decorrelateDenseWeightSmoothed.setTargetValue (lastDecorrelateMode == static_cast<int> (DecorrelateMode::velvetDense) ? 1.0f : 0.0f);
    decorrelateSparseWeightSmoothed.setTargetValue (lastDecorrelateMode == static_cast<int> (DecorrelateMode::velvetSparse) ? 1.0f : 0.0f);

    haasEnableSmoothed.setTargetValue (lastHaasEnabled ? 1.0f : 0.0f);
    haasPinSmoothed.setTargetValue ((lastHaasEnabled && ! lastDecorrelateEnabled) ? 1.0f : 0.0f);

    midBassApWeightSmoothed.setTargetValue ((phaseMatched && bassMonoEnabled && ! phaseMatchBypassedForTests) ? 1.0f : 0.0f);
    midHighApWeightSmoothed.setTargetValue ((phaseMatched && highSplitEnabled && ! phaseMatchBypassedForTests) ? 1.0f : 0.0f);

    highSplitBlendSmoothed.setTargetValue (highSplitEnabled ? 1.0f : 0.0f);
    safetyModeBlendSmoothed.setTargetValue (lastSafetyMode == static_cast<int> (SafetyMode::dynamic) ? 1.0f : 0.0f);
    widthCompGainSmoothed.setTargetValue (computeWidthCompensationTarget());
    monoAuditionSmoothed.setTargetValue (lastMonoAuditionEnabled ? 1.0f : 0.0f);
}

float FirmamentEngine::computeWidthCompensationTarget() const noexcept
{
    if (! lastWidthCompensationEnabled)
        return 1.0f;

    // Equal-power width compensation (research-stereo-imaging.md 2.1): with
    // a = (1+w)/2, b = (1-w)/2, the makeup g = 1/sqrt(a^2 + b^2) is
    // normalised so g(w = 1) = 1 exactly. Computed from the broadband width
    // parameter only (documented multiband limitation).
    const auto w = lastWidthPercent * 0.01f;
    const auto a = (1.0f + w) * 0.5f;
    const auto b = (1.0f - w) * 0.5f;
    return 1.0f / std::sqrt (juce::jmax (1.0e-6f, a * a + b * b));
}

void FirmamentEngine::setWidthPercent (float newWidthPercent)
{
    lastWidthPercent = newWidthPercent;
    widthSmoothed.setTargetValue (newWidthPercent * 0.01f);
    widthCompGainSmoothed.setTargetValue (computeWidthCompensationTarget());
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

    refreshCrossfadeTargets();

    // See getLatencySamples()/linearPhaseCommanded's comments.
    linearPhaseCommanded.store (lastBassMonoMode == static_cast<int> (BassMonoMode::linearPhase) && lastBassMonoHz > 0.0f,
                                std::memory_order_relaxed);
}

void FirmamentEngine::setAutoMonoSafetyEnabled (bool shouldBeEnabled)
{
    lastAutoMonoSafetyEnabled = shouldBeEnabled;
    autoMonoSafetyAmountSmoothed.setTargetValue (shouldBeEnabled ? 1.0f : 0.0f);
}

void FirmamentEngine::setHaasEnabled (bool shouldBeEnabled)
{
    lastHaasEnabled = shouldBeEnabled;
    refreshCrossfadeTargets();
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

void FirmamentEngine::setAutoMonoSafetyFloorDb (float newFloorDb)
{
    lastAutoMonoSafetyFloorDb = newFloorDb;
    autoMonoSafetyFloorGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (newFloorDb));
}

void FirmamentEngine::setAutoMonoSafetyMultibandEnabled (bool shouldBeEnabled)
{
    lastAutoMonoSafetyMultibandEnabled = shouldBeEnabled;
}

void FirmamentEngine::setDecorrelateEnabled (bool shouldBeEnabled)
{
    lastDecorrelateEnabled = shouldBeEnabled;
    refreshCrossfadeTargets();
}

void FirmamentEngine::setDecorrelateAmountPercent (float newAmountPercent)
{
    lastDecorrelateAmountPercent = newAmountPercent;
    decorrelateAmountSmoothed.setTargetValue (newAmountPercent * 0.01f);
}

void FirmamentEngine::setDecorrelateMode (int newMode)
{
    lastDecorrelateMode = juce::jlimit (0, 2, newMode);
    refreshCrossfadeTargets();
}

void FirmamentEngine::setBassMonoMode (int newMode)
{
    lastBassMonoMode = juce::jlimit (0, 2, newMode);
    refreshCrossfadeTargets();

    // See getLatencySamples()/linearPhaseCommanded's comments.
    linearPhaseCommanded.store (lastBassMonoMode == static_cast<int> (BassMonoMode::linearPhase) && lastBassMonoHz > 0.0f,
                                std::memory_order_relaxed);
}

void FirmamentEngine::setHighSplitFrequencyHz (float newFrequencyHz)
{
    lastHighSplitHz = newFrequencyHz;

    // Same 0 Hz "off" sentinel convention as the bass-mono crossover.
    if (newFrequencyHz > 0.0f)
        highSplitFrequencySmoothed.setTargetValue (juce::jmax (5.0f, newFrequencyHz));

    refreshCrossfadeTargets();
}

void FirmamentEngine::setHighWidthPercent (float newHighWidthPercent)
{
    lastHighWidthPercent = newHighWidthPercent;
    highWidthSmoothed.setTargetValue (newHighWidthPercent * 0.01f);
}

void FirmamentEngine::setSafetyMode (int newMode)
{
    lastSafetyMode = juce::jlimit (0, 1, newMode);
    refreshCrossfadeTargets();
}

void FirmamentEngine::setWidthCompensationEnabled (bool shouldBeEnabled)
{
    lastWidthCompensationEnabled = shouldBeEnabled;
    widthCompGainSmoothed.setTargetValue (computeWidthCompensationTarget());
}

void FirmamentEngine::setMonoAuditionEnabled (bool shouldBeEnabled)
{
    lastMonoAuditionEnabled = shouldBeEnabled;
    refreshCrossfadeTargets();
}

void FirmamentEngine::setPhaseMatchBypassedForTests (bool shouldBypass)
{
    phaseMatchBypassedForTests = shouldBypass;
    refreshCrossfadeTargets();
}

int FirmamentEngine::getLatencySamples() const noexcept
{
    // Reads the atomic snapshot rather than lastBassMonoMode/lastBassMonoHz
    // directly - see this method's declaration comment and
    // linearPhaseCommanded's comment in the header.
    return linearPhaseCommanded.load (std::memory_order_relaxed) ? linearPhase.getLatencySamples() : 0;
}

// ---------------------------------------------------------------------------

void FirmamentEngine::process (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto numSamples = block.getNumSamples();

    if (numSamples == 0)
        return;

    // The M/S encode/decode below is only meaningful for a genuine stereo
    // pair; the processor guarantees this (duplicating mono input to both
    // channels before calling process(), see PluginProcessor.cpp), but this
    // guard makes the engine safe to call standalone with any other channel
    // count too.
    if (block.getNumChannels() != 2)
        return;

    if (preparedMaxBlockSize <= 0)
        return;

    // Oversized-block guard (v0.3.0, suite convention): hosts occasionally
    // deliver blocks larger than the maximum they declared. A real chunked
    // clamp (not just an assert, which compiles out in Release) keeps every
    // internal scratch buffer within its prepared size.
    const auto maxChunk = static_cast<size_t> (preparedMaxBlockSize);
    size_t offset = 0;

    while (offset < numSamples)
    {
        const auto chunkSize = juce::jmin (numSamples - offset, maxChunk);
        auto chunk = block.getSubBlock (offset, chunkSize);
        processChunk (chunk);
        offset += chunkSize;
    }
}

void FirmamentEngine::processChunk (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto numSamples = block.getNumSamples();

    const bool bassMonoEnabled = lastBassMonoHz > 0.0f;
    const bool multibandSafetyEnabled = lastAutoMonoSafetyMultibandEnabled && bassMonoEnabled;

    // v0.3.0 Linear Phase transition state machine: any change to/from the
    // Linear Phase path fades the output down over ~5 ms, swaps the active
    // path (resetting the FIR tier's state so it starts clean), then fades
    // back up. The latency change itself is reported by the processor on
    // the message thread; the short mute is the documented cost of a
    // mid-stream PDC change (brief, parameter table).
    const bool linearPhaseDesired = lastBassMonoMode == static_cast<int> (BassMonoMode::linearPhase)
                                    && bassMonoEnabled;

    if (linearPhaseDesired != linearPhaseActive)
    {
        linearPhaseFadeSmoothed.setTargetValue (0.0f);

        if (linearPhaseFadeSmoothed.getCurrentValue() <= 0.0f)
        {
            linearPhaseActive = linearPhaseDesired;
            linearPhase.reset();
            linearPhaseFadeSmoothed.setTargetValue (1.0f);
        }
    }
    else
    {
        linearPhaseFadeSmoothed.setTargetValue (1.0f);
    }

    // Coefficient recomputation involves a tan() call, so crossover
    // frequencies are smoothed and re-derived once per block rather than per
    // sample. This runs unconditionally (not just while the stages are
    // enabled) so every filter's coefficients - and, in the per-sample loop
    // below, its internal TPT state - keep tracking the live signal even
    // while a section is disabled (the "always process, conditionally use"
    // pattern, GitHub issue #12). The frequency smoothers' targets only ever
    // move while their stages are commanded on, so while disabled they
    // simply hold at the last commanded (or prepare()-seeded) frequency.
    const auto freqHz = clampBelowNyquist (bassMonoFrequencySmoothed.skip (static_cast<int> (numSamples)), sampleRate);
    bassMonoCrossover.setCutoffFrequency (freqHz);
    leftMultibandCrossover.setCutoffFrequency (freqHz);
    rightMultibandCrossover.setCutoffFrequency (freqHz);

    // The Phase Matched companion allpass is recomputed here, in the same
    // once-per-block update, from the same smoothed fc snapshot as the LR4
    // sections (binding, brief 3.2b) - as is the Linear Phase kernel target
    // (the actual FIR recompute happens on the message thread, coalesced;
    // see LinearPhaseCrossover.h).
    midBassAllpass.setCutoffFrequency (freqHz);
    linearPhase.setTargetCutoffFrequency (freqHz);

    // v0.3.0 high split: same smoothing/recompute discipline; the internal
    // clamp keeps it at least an octave above the bass-mono corner while
    // both are engaged (documented in the manual).
    const auto rawHighSplitHz = highSplitFrequencySmoothed.skip (static_cast<int> (numSamples));
    const auto highSplitHz = clampBelowNyquist (bassMonoEnabled ? juce::jmax (rawHighSplitHz, 2.0f * freqHz)
                                                                : rawHighSplitHz,
                                                sampleRate);
    sideHighSplitCrossover.setCutoffFrequency (highSplitHz);
    leftHighSplitInputCrossover.setCutoffFrequency (highSplitHz);
    rightHighSplitInputCrossover.setCutoffFrequency (highSplitHz);
    midHighAllpass.setCutoffFrequency (highSplitHz);
    lowBandHighAllpass.setCutoffFrequency (highSplitHz);

    auto* left = block.getChannelPointer (0);
    auto* right = block.getChannelPointer (1);

    // v0.3.0 Linear Phase precompute: the FIR split and matching Mid delay
    // are block operations (juce::dsp::Convolution processes contexts, not
    // samples), so while the Linear Phase path is active the scrubbed,
    // encoded Side/Mid streams are computed up front and the per-sample loop
    // below consumes the split results. Only runs in Linear Phase mode -
    // the convolution stays idle (and costs nothing) otherwise.
    if (linearPhaseActive)
    {
        for (size_t i = 0; i < numSamples; ++i)
        {
            const auto leftSample = std::isfinite (left[i]) ? left[i] : 0.0f;
            const auto rightSample = std::isfinite (right[i]) ? right[i] : 0.0f;
            const auto encoded = MidSideCodec::encode (leftSample, rightSample);
            lpSideIn[i] = encoded.side;
            lpMidIn[i] = encoded.mid;
        }

        linearPhase.processBlock (lpSideIn.data(), lpSideLow.data(), lpSideHigh.data(),
                                  lpMidIn.data(), lpMidDelayed.data(), static_cast<int> (numSamples));
    }

    const auto maxDelaySamples = static_cast<float> (haasDelayLine.getMaximumDelayInSamples());

    for (size_t i = 0; i < numSamples; ++i)
    {
        // Width/Low Width/High Width are plain multiplicative scales with no
        // coefficients to recompute, so they are interpolated
        // sample-accurately; each is always advanced so it is caught up with
        // its target the instant its stage is enabled.
        const auto widthProportion = widthSmoothed.getNextValue();
        const auto lowWidthProportion = lowWidthSmoothed.getNextValue();
        const auto floorGain = autoMonoSafetyFloorGainSmoothed.getNextValue();
        const auto decorrelateMix = decorrelateAmountSmoothed.getNextValue();
        const auto highWidthProportion = highWidthSmoothed.getNextValue();

        // v0.3.0 crossfade weights (settled values are exactly 0/1, see
        // blendSelect()).
        const auto highSplitWeight = highSplitBlendSmoothed.getNextValue();
        const auto midBassApWeight = midBassApWeightSmoothed.getNextValue();
        const auto midHighApWeight = midHighApWeightSmoothed.getNextValue();
        const auto safetyModeWeight = safetyModeBlendSmoothed.getNextValue();
        const auto decorrelateEnableWeight = decorrelateEnableSmoothed.getNextValue();
        const auto classicWeight = decorrelateClassicWeightSmoothed.getNextValue();
        const auto denseWeight = decorrelateDenseWeightSmoothed.getNextValue();
        const auto sparseWeight = decorrelateSparseWeightSmoothed.getNextValue();
        const auto haasEnableWeight = haasEnableSmoothed.getNextValue();
        const auto haasPinWeight = haasPinSmoothed.getNextValue();
        const auto widthCompGain = widthCompGainSmoothed.getNextValue();
        const auto linearPhaseFade = linearPhaseFadeSmoothed.getNextValue();

        // A single NaN/Inf input sample must never be allowed to reach an
        // IIR filter's internal state (it would poison it indefinitely).
        // Scrubbing here keeps that failure mode local to the single bad
        // sample instead of the rest of the stream.
        const auto leftSample = std::isfinite (left[i]) ? left[i] : 0.0f;
        const auto rightSample = std::isfinite (right[i]) ? right[i] : 0.0f;

        // Correlation meters / Auto Mono Safety: leaky-integrated running
        // estimates of the plugin's *input* L/R correlation, updated every
        // sample regardless of whether any consumer is enabled. Deriving
        // them from the raw scrubbed input keeps them a direct read of the
        // source material's own mono-compatibility risk and avoids any
        // feedback loop with the Side scaling computed from them below.
        const auto coeff = correlationSmoothingCoeff;
        correlationSumLR = coeff * correlationSumLR + (1.0 - coeff) * (static_cast<double> (leftSample) * static_cast<double> (rightSample));
        correlationSumLL = coeff * correlationSumLL + (1.0 - coeff) * (static_cast<double> (leftSample) * static_cast<double> (leftSample));
        correlationSumRR = coeff * correlationSumRR + (1.0 - coeff) * (static_cast<double> (rightSample) * static_cast<double> (rightSample));

        // Per-band input splits (always running): raw L/R at the bass-mono
        // corner (v0.2.0), then the above-corner content again at the high
        // split (v0.3.0) for the third band.
        float leftLow = 0.0f, leftHigh = 0.0f;
        float rightLow = 0.0f, rightHigh = 0.0f;
        leftMultibandCrossover.processSample (0, leftSample, leftLow, leftHigh);
        rightMultibandCrossover.processSample (0, rightSample, rightLow, rightHigh);

        correlationSumLRLow = coeff * correlationSumLRLow + (1.0 - coeff) * (static_cast<double> (leftLow) * static_cast<double> (rightLow));
        correlationSumLLLow = coeff * correlationSumLLLow + (1.0 - coeff) * (static_cast<double> (leftLow) * static_cast<double> (leftLow));
        correlationSumRRLow = coeff * correlationSumRRLow + (1.0 - coeff) * (static_cast<double> (rightLow) * static_cast<double> (rightLow));

        correlationSumLRHigh = coeff * correlationSumLRHigh + (1.0 - coeff) * (static_cast<double> (leftHigh) * static_cast<double> (rightHigh));
        correlationSumLLHigh = coeff * correlationSumLLHigh + (1.0 - coeff) * (static_cast<double> (leftHigh) * static_cast<double> (leftHigh));
        correlationSumRRHigh = coeff * correlationSumRRHigh + (1.0 - coeff) * (static_cast<double> (rightHigh) * static_cast<double> (rightHigh));

        float leftMidBand = 0.0f, leftTopBand = 0.0f;
        float rightMidBand = 0.0f, rightTopBand = 0.0f;
        leftHighSplitInputCrossover.processSample (0, leftHigh, leftMidBand, leftTopBand);
        rightHighSplitInputCrossover.processSample (0, rightHigh, rightMidBand, rightTopBand);

        leakyUpdate (correlationSumLRMid, coeff, static_cast<double> (leftMidBand) * static_cast<double> (rightMidBand));
        leakyUpdate (correlationSumLLMid, coeff, static_cast<double> (leftMidBand) * static_cast<double> (leftMidBand));
        leakyUpdate (correlationSumRRMid, coeff, static_cast<double> (rightMidBand) * static_cast<double> (rightMidBand));
        leakyUpdate (correlationSumLRTop, coeff, static_cast<double> (leftTopBand) * static_cast<double> (rightTopBand));
        leakyUpdate (correlationSumLLTop, coeff, static_cast<double> (leftTopBand) * static_cast<double> (leftTopBand));
        leakyUpdate (correlationSumRRTop, coeff, static_cast<double> (rightTopBand) * static_cast<double> (rightTopBand));

        // v0.3.0 fast (30 ms) estimator set for the Dynamic safety path -
        // same structure, always running so mode switches start warm.
        const auto fastCoeff = fastCorrelationSmoothingCoeff;
        leakyUpdate (fastSumLR, fastCoeff, static_cast<double> (leftSample) * static_cast<double> (rightSample));
        leakyUpdate (fastSumLL, fastCoeff, static_cast<double> (leftSample) * static_cast<double> (leftSample));
        leakyUpdate (fastSumRR, fastCoeff, static_cast<double> (rightSample) * static_cast<double> (rightSample));
        leakyUpdate (fastSumLRLow, fastCoeff, static_cast<double> (leftLow) * static_cast<double> (rightLow));
        leakyUpdate (fastSumLLLow, fastCoeff, static_cast<double> (leftLow) * static_cast<double> (leftLow));
        leakyUpdate (fastSumRRLow, fastCoeff, static_cast<double> (rightLow) * static_cast<double> (rightLow));
        leakyUpdate (fastSumLRHigh, fastCoeff, static_cast<double> (leftHigh) * static_cast<double> (rightHigh));
        leakyUpdate (fastSumLLHigh, fastCoeff, static_cast<double> (leftHigh) * static_cast<double> (leftHigh));
        leakyUpdate (fastSumRRHigh, fastCoeff, static_cast<double> (rightHigh) * static_cast<double> (rightHigh));
        leakyUpdate (fastSumLRMid, fastCoeff, static_cast<double> (leftMidBand) * static_cast<double> (rightMidBand));
        leakyUpdate (fastSumLLMid, fastCoeff, static_cast<double> (leftMidBand) * static_cast<double> (leftMidBand));
        leakyUpdate (fastSumRRMid, fastCoeff, static_cast<double> (rightMidBand) * static_cast<double> (rightMidBand));
        leakyUpdate (fastSumLRTop, fastCoeff, static_cast<double> (leftTopBand) * static_cast<double> (rightTopBand));
        leakyUpdate (fastSumLLTop, fastCoeff, static_cast<double> (leftTopBand) * static_cast<double> (leftTopBand));
        leakyUpdate (fastSumRRTop, fastCoeff, static_cast<double> (rightTopBand) * static_cast<double> (rightTopBand));

        // Energy-gated estimates (see updateGatedCorrelation()): the gate
        // rule (brief 3.8) applies to the *display* meter values and the
        // fast Dynamic-guard detectors. The Smooth control path deliberately
        // keeps the raw v0.2.0 ratio (brief 3.4: "Keep the existing 300 ms
        // leaky integrator ... as the 'Smooth' (legacy, default) control
        // path - bit-identical") - the raw ratios are computed inline in the
        // safety branches below, textually as in v0.2.0.
        updateGatedCorrelation (correlationSumLR, correlationSumLL, correlationSumRR, gatedCorrelation, coeff);
        updateGatedCorrelation (correlationSumLRLow, correlationSumLLLow, correlationSumRRLow, gatedCorrelationLow, coeff);
        updateGatedCorrelation (correlationSumLRHigh, correlationSumLLHigh, correlationSumRRHigh, gatedCorrelationHigh, coeff);
        updateGatedCorrelation (correlationSumLRMid, correlationSumLLMid, correlationSumRRMid, gatedCorrelationMid, coeff);
        updateGatedCorrelation (correlationSumLRTop, correlationSumLLTop, correlationSumRRTop, gatedCorrelationTop, coeff);
        updateGatedCorrelation (fastSumLR, fastSumLL, fastSumRR, gatedFastCorrelation, fastCoeff);
        updateGatedCorrelation (fastSumLRLow, fastSumLLLow, fastSumRRLow, gatedFastCorrelationLow, fastCoeff);
        updateGatedCorrelation (fastSumLRHigh, fastSumLLHigh, fastSumRRHigh, gatedFastCorrelationHigh, fastCoeff);
        updateGatedCorrelation (fastSumLRMid, fastSumLLMid, fastSumRRMid, gatedFastCorrelationMid, fastCoeff);
        updateGatedCorrelation (fastSumLRTop, fastSumLLTop, fastSumRRTop, gatedFastCorrelationTop, fastCoeff);

        // Dynamic safety gain smoothers (always running - warm on mode
        // switch): static map on the fast estimate, then the dedicated
        // asymmetric one-pole (brief 3.4).
        updateDynamicGain (dynamicGainBroadband, computeAutoMonoSafetyGain (gatedFastCorrelation, floorGain), safetyAttackCoeff, safetyReleaseCoeff);
        updateDynamicGain (dynamicGainLow, computeAutoMonoSafetyGain (gatedFastCorrelationLow, floorGain), safetyAttackCoeff, safetyReleaseCoeff);
        updateDynamicGain (dynamicGainHigh, computeAutoMonoSafetyGain (gatedFastCorrelationHigh, floorGain), safetyAttackCoeff, safetyReleaseCoeff);
        updateDynamicGain (dynamicGainMid, computeAutoMonoSafetyGain (gatedFastCorrelationMid, floorGain), safetyAttackCoeff, safetyReleaseCoeff);
        updateDynamicGain (dynamicGainTop, computeAutoMonoSafetyGain (gatedFastCorrelationTop, floorGain), safetyAttackCoeff, safetyReleaseCoeff);

        const auto encoded = MidSideCodec::encode (leftSample, rightSample);

        // The bass-mono crossover is always run against the live (unscaled)
        // Side signal - even while disabled or while the Linear Phase path
        // is active - so its state stays synced with the live input; only
        // the split output is conditionally used below (issue #12 pattern).
        float lowBand = 0.0f;
        float highBand = 0.0f;
        bassMonoCrossover.processSample (0, encoded.side, lowBand, highBand);

        // Band inputs for the width stage: minimum-phase LR4 bands, or the
        // FIR complementary split while Linear Phase is active.
        const auto lowIn = linearPhaseActive ? lpSideLow[i] : lowBand;
        const auto restIn = linearPhaseActive ? lpSideHigh[i] : highBand;

        // v0.3.0 high split (always processed): splits the above-bass-mono
        // content into mid/top bands; the low band's AP2(HighSplitFreq)
        // keeps the 3-band sum phase-coherent (brief 3.3).
        const auto restForHighSplit = bassMonoEnabled ? restIn : encoded.side;
        float midBandS = 0.0f, topBandS = 0.0f;
        sideHighSplitCrossover.processSample (0, restForHighSplit, midBandS, topBandS);
        const auto lowApOut = lowBandHighAllpass.processSample (0, lowIn);

        const auto autoMonoAmount = autoMonoSafetyAmountSmoothed.getNextValue();

        float side;
        float side3 = 0.0f;

        if (bassMonoEnabled)
        {
            // Splitting the *unscaled* Side signal into bands and scaling
            // each independently before summing commutes exactly with
            // scale-then-filter (both are linear operations), so at the
            // default Low Width of 0% this reproduces the v0.1 "bass mono
            // forces the low band to silence" behaviour precisely.
            if (multibandSafetyEnabled)
            {
                // Smooth path: the raw per-band v0.2.0 ratios (deliberately
                // ungated - see the estimator comment above); Dynamic path:
                // the ballistic per-band gains; crossfaded on mode changes.
                const auto denominatorLow = std::sqrt (correlationSumLLLow * correlationSumRRLow + correlationEpsilon);
                const auto correlationLow = static_cast<float> (juce::jlimit (-1.0, 1.0, correlationSumLRLow / denominatorLow));
                const auto safetyGainLow = blendSelect (computeAutoMonoSafetyGain (correlationLow, floorGain), dynamicGainLow, safetyModeWeight);
                const auto effectiveSafetyGainLow = 1.0f + autoMonoAmount * (safetyGainLow - 1.0f);

                const auto denominatorHigh = std::sqrt (correlationSumLLHigh * correlationSumRRHigh + correlationEpsilon);
                const auto correlationHigh = static_cast<float> (juce::jlimit (-1.0, 1.0, correlationSumLRHigh / denominatorHigh));
                const auto safetyGainHigh = blendSelect (computeAutoMonoSafetyGain (correlationHigh, floorGain), dynamicGainHigh, safetyModeWeight);
                const auto effectiveSafetyGainHigh = 1.0f + autoMonoAmount * (safetyGainHigh - 1.0f);

                side = lowIn * lowWidthProportion * effectiveSafetyGainLow + restIn * widthProportion * effectiveSafetyGainHigh;

                if (highSplitWeight > 0.0f)
                {
                    const auto denominatorMid = std::sqrt (correlationSumLLMid * correlationSumRRMid + correlationEpsilon);
                    const auto correlationMid = static_cast<float> (juce::jlimit (-1.0, 1.0, correlationSumLRMid / denominatorMid));
                    const auto safetyGainMid = blendSelect (computeAutoMonoSafetyGain (correlationMid, floorGain), dynamicGainMid, safetyModeWeight);
                    const auto effectiveSafetyGainMid = 1.0f + autoMonoAmount * (safetyGainMid - 1.0f);

                    const auto denominatorTop = std::sqrt (correlationSumLLTop * correlationSumRRTop + correlationEpsilon);
                    const auto correlationTop = static_cast<float> (juce::jlimit (-1.0, 1.0, correlationSumLRTop / denominatorTop));
                    const auto safetyGainTop = blendSelect (computeAutoMonoSafetyGain (correlationTop, floorGain), dynamicGainTop, safetyModeWeight);
                    const auto effectiveSafetyGainTop = 1.0f + autoMonoAmount * (safetyGainTop - 1.0f);

                    side3 = lowApOut * lowWidthProportion * effectiveSafetyGainLow
                            + midBandS * widthProportion * effectiveSafetyGainMid
                            + topBandS * highWidthProportion * effectiveSafetyGainTop;
                }
            }
            else
            {
                side = lowIn * lowWidthProportion + restIn * widthProportion;

                if (highSplitWeight > 0.0f)
                    side3 = lowApOut * lowWidthProportion + midBandS * widthProportion + topBandS * highWidthProportion;
            }
        }
        else
        {
            // Single-band mode (bass-mono off): Width alone scales the whole
            // Side signal, exactly as in v0.1/v0.2.0. With the high split
            // engaged the Side splits into mid/top (2-band imaging without
            // forced bass-mono).
            side = encoded.side * widthProportion;

            if (highSplitWeight > 0.0f)
                side3 = midBandS * widthProportion + topBandS * highWidthProportion;
        }

        // v0.3.0: sentinel-crossfaded high split. With the sentinel at 0 the
        // weight is exactly 0 and the band sum path is bit-identical to the
        // two-band expressions above (brief 6.1a/6.12).
        side = blendSelect (side, side3, highSplitWeight);

        // Auto Mono Safety (broadband path, v0.2.0 discipline unchanged):
        // blended between full bypass (1.0) and fully engaged via the
        // smoothed 0..1 amount. Skipped entirely when the multiband path
        // above already applied its own per-band gains.
        if (! (bassMonoEnabled && multibandSafetyEnabled))
        {
            // Smooth path: the raw v0.2.0 broadband ratio (deliberately
            // ungated - brief 3.4 pins this control path bit-identical);
            // Dynamic path: the ballistic gain; crossfaded on mode changes.
            const auto denominator = std::sqrt (correlationSumLL * correlationSumRR + correlationEpsilon);
            const auto correlationEstimate = static_cast<float> (juce::jlimit (-1.0, 1.0, correlationSumLR / denominator));

            const auto safetyGain = blendSelect (computeAutoMonoSafetyGain (correlationEstimate, floorGain), dynamicGainBroadband, safetyModeWeight);
            const auto effectiveSafetyGain = 1.0f + autoMonoAmount * (safetyGain - 1.0f);
            side *= effectiveSafetyGain;
        }

        // Mid path: dry in Classic mode; AP2(bass) [* AP2(highSplit)] in
        // Phase Matched mode (both allpasses always process for warm state);
        // the matching N/2 delay while Linear Phase is active.
        float mid = linearPhaseActive ? lpMidDelayed[i] : encoded.mid;
        const auto midBassApOut = midBassAllpass.processSample (0, mid);
        mid = blendSelect (mid, midBassApOut, midBassApWeight);
        const auto midHighApOut = midHighAllpass.processSample (0, mid);
        mid = blendSelect (mid, midHighApOut, midHighApWeight);

        const auto decoded = MidSideCodec::decode (mid, side);

        // v0.3.0 equal-power width compensation: exactly 1.0 while off (a
        // bit-exact identity multiply), the smoothed makeup while on.
        const auto compensatedLeft = decoded.left * widthCompGain;
        const auto compensatedRight = decoded.right * widthCompGain;

        // Haas Mode (v0.3.0 polish): per-sample smoothed delay time applied
        // through a Lagrange 3rd-order interpolated line - integer-sample
        // delays (including the pinned 0) remain exact. The "pin" weight is
        // 0 while Haas is off or Decorrelate is engaged (mutual
        // exclusivity), so the line is an exact passthrough then. Always
        // pushed/popped so re-enabling never starts from stale history.
        const auto haasDelaySamples = juce::jlimit (0.0f, maxDelaySamples,
                                                    haasTimeMsSmoothed.getNextValue() * 0.001f * static_cast<float> (sampleRate))
                                      * haasPinWeight;
        haasDelayLine.setDelay (haasDelaySamples);
        haasDelayLine.pushSample (0, compensatedRight);
        const auto haasRight = haasDelayLine.popSample (0);

        // v0.3.0: the enable itself is a crossfade, not an instant gate
        // (survey 4.7). While Decorrelate is engaged the delay is pinned to
        // 0, so this blend resolves to the dry sample bit-exactly.
        const auto rightAfterHaas = blendSelect (compensatedRight, haasRight, haasEnableWeight);

        // Decorrelate paths - all three always process so mode switches
        // start from warm state ("always process, conditionally use"):
        // Classic = v0.2.0 R-only allpass cascade;
        // Velvet Dense/Sparse = symmetric complementary OVN pairs on L & R.
        float classicDecorrelated = compensatedRight;

        for (auto& stage : decorrelateAllpassStages)
            classicDecorrelated = stage.processSample (classicDecorrelated);

        const auto velvetDenseL = velvetDenseLeft.processSample (compensatedLeft);
        const auto velvetDenseR = velvetDenseRight.processSample (compensatedRight);
        const auto velvetSparseL = velvetSparseLeft.processSample (compensatedLeft);
        const auto velvetSparseR = velvetSparseRight.processSample (compensatedRight);

        float decorrelatedL;
        float decorrelatedR;

        if (classicWeight >= 1.0f)
        {
            // v0.2.0 Classic, bit-exact: Left untouched, Right blended
            // against the cascade by the smoothed amount.
            decorrelatedL = compensatedLeft;
            decorrelatedR = compensatedRight + decorrelateMix * (classicDecorrelated - compensatedRight);
        }
        else
        {
            // Velvet modes: mono-safe projection of the symmetric
            // complementary topology (deviation from the brief's raw
            // per-channel wet mix, documented in docs/architecture.md).
            //
            // The brief's L' = (1-d)L + d*VND_A(L), R' = (1-d)R + d*VND_B(R)
            // decomposes in M/S as
            //     M'' = (1-d)M + d*(A(L)+B(R))/2
            //     S'' = (1-d)S + d*(A(L)-B(R))/2
            // and the published OVN pair sum A+B carries a real ~17 dB
            // third-octave notch (~320 Hz @48 kHz, measurable from the
            // DAFx-18 Table 1 coefficients), so the raw topology cannot meet
            // the binding <= 3 dB mono fold-down assertion (brief 6.7) at
            // 100% amount. Firmament therefore keeps the *widening* half of
            // the topology - S' = S'' bit-for-bit - and pins M' = M (dry),
            // which makes every velvet setting mono-sum invariant BY
            // CONSTRUCTION ("provably mono-safe by construction", brief
            // section 1) instead of merely low-cost. Equivalently, per
            // channel:
            //     v  = (A(L) - B(R))/2 - S
            //     L' = L + d*v,  R' = R - d*v
            // which is an exact identity at d = 0 and fully replaces Side
            // with the velvet-diffused difference signal at d = 1.
            const auto sideNow = 0.5f * (compensatedLeft - compensatedRight);

            const auto denseInjection = 0.5f * (velvetDenseL - velvetDenseR) - sideNow;
            const auto sparseInjection = 0.5f * (velvetSparseL - velvetSparseR) - sideNow;

            if (denseWeight >= 1.0f)
            {
                decorrelatedL = compensatedLeft + decorrelateMix * denseInjection;
                decorrelatedR = compensatedRight - decorrelateMix * denseInjection;
            }
            else if (sparseWeight >= 1.0f)
            {
                decorrelatedL = compensatedLeft + decorrelateMix * sparseInjection;
                decorrelatedR = compensatedRight - decorrelateMix * sparseInjection;
            }
            else
            {
                // Mid-crossfade: the linear same-length weight ramps sum
                // to 1.
                const auto classicR = compensatedRight + decorrelateMix * (classicDecorrelated - compensatedRight);
                const auto denseL = compensatedLeft + decorrelateMix * denseInjection;
                const auto denseR = compensatedRight - decorrelateMix * denseInjection;
                const auto sparseL = compensatedLeft + decorrelateMix * sparseInjection;
                const auto sparseR = compensatedRight - decorrelateMix * sparseInjection;

                decorrelatedL = classicWeight * compensatedLeft + denseWeight * denseL + sparseWeight * sparseL;
                decorrelatedR = classicWeight * classicR + denseWeight * denseR + sparseWeight * sparseR;
            }
        }

        // Master Decorrelate enable: a smoothed 0..1 crossfade (fixes the
        // v0.2.0 instant gate, survey 4.7). Decorrelate wins over Haas while
        // engaged (the delay is pinned to 0 then, so rightAfterHaas has
        // already collapsed to the dry sample).
        const auto leftOut = blendSelect (compensatedLeft, decorrelatedL, decorrelateEnableWeight);
        const auto rightOut = blendSelect (rightAfterHaas, decorrelatedR, decorrelateEnableWeight);

        // The Linear Phase transition fade (exactly 1.0 in steady state - a
        // bit-exact identity multiply).
        left[i] = leftOut * linearPhaseFade;
        right[i] = rightOut * linearPhaseFade;
    }

    lastCorrelation = gatedCorrelation;
    lastCorrelationLow = gatedCorrelationLow;
    lastCorrelationHigh = gatedCorrelationHigh;
    lastCorrelationMidBand = gatedCorrelationMid;
    lastCorrelationHighBand = gatedCorrelationTop;

    juce::dsp::ProcessContextReplacing<float> context (block);
    outputGain.process (context);

    // v0.3.0 post-trim stage: Mono Audition (post-everything - it *is* the
    // mono fold-down, so it sits after the output trim) and the output
    // (post-processing) correlation meter measured on the final samples.
    for (size_t i = 0; i < numSamples; ++i)
    {
        const auto auditionWeight = monoAuditionSmoothed.getNextValue();

        if (auditionWeight > 0.0f)
        {
            const auto mono = 0.5f * (left[i] + right[i]);
            left[i] = blendSelect (left[i], mono, auditionWeight);
            right[i] = blendSelect (right[i], mono, auditionWeight);
        }

        const auto outCoeff = correlationSmoothingCoeff;
        leakyUpdate (outputSumLR, outCoeff, static_cast<double> (left[i]) * static_cast<double> (right[i]));
        leakyUpdate (outputSumLL, outCoeff, static_cast<double> (left[i]) * static_cast<double> (left[i]));
        leakyUpdate (outputSumRR, outCoeff, static_cast<double> (right[i]) * static_cast<double> (right[i]));
        updateGatedCorrelation (outputSumLR, outputSumLL, outputSumRR, gatedOutputCorrelation, outCoeff);
    }

    lastOutputCorrelation = gatedOutputCorrelation;
}
