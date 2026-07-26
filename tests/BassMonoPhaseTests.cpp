// v0.3.0 bass-mono mode selector tests (binding brief, sections 6.2/6.3):
// Phase Matched mode's phase-identity/flatness guarantees and Linear Phase
// mode's perfect-reconstruction/attenuation/constant-group-delay guarantees.
//
// Measurement note: the whole chain is LTI (no nonlinearity anywhere), so an
// impulse measures exactly the same transfer function a deconvolved log-sine
// sweep would, without deconvolution machinery - the assertions below are
// the brief's, verbatim.

#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <vector>

namespace
{
    constexpr int fftOrder = 15; // 32768-point analysis
    constexpr int fftSize = 1 << fftOrder;

    juce::dsp::ProcessSpec makeSpec (double sampleRate, int blockSize = 512)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        return spec;
    }

    // Renders `length` output samples of both channels for an input that is
    // zero except for an impulse of `leftAmplitude`/`rightAmplitude` at
    // sample 0.
    std::pair<std::vector<float>, std::vector<float>> renderImpulseResponse (FirmamentEngine& engine,
                                                                             float leftAmplitude,
                                                                             float rightAmplitude,
                                                                             int length,
                                                                             int blockSize = 512)
    {
        std::vector<float> outLeft, outRight;
        outLeft.reserve (static_cast<size_t> (length));
        outRight.reserve (static_cast<size_t> (length));

        juce::AudioBuffer<float> buffer (2, blockSize);
        bool impulseSent = false;

        while (static_cast<int> (outLeft.size()) < length)
        {
            buffer.clear();

            if (! impulseSent)
            {
                buffer.setSample (0, 0, leftAmplitude);
                buffer.setSample (1, 0, rightAmplitude);
                impulseSent = true;
            }

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);

            const auto* left = buffer.getReadPointer (0);
            const auto* right = buffer.getReadPointer (1);
            outLeft.insert (outLeft.end(), left, left + blockSize);
            outRight.insert (outRight.end(), right, right + blockSize);
        }

        outLeft.resize (static_cast<size_t> (length));
        outRight.resize (static_cast<size_t> (length));
        return { std::move (outLeft), std::move (outRight) };
    }

    // Complex spectrum of a real signal (rectangular window - correct for
    // full impulse responses that have decayed inside the frame).
    std::vector<std::complex<float>> complexSpectrum (const std::vector<float>& samples)
    {
        jassert (static_cast<int> (samples.size()) <= fftSize);

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> data (static_cast<size_t> (fftSize) * 2, 0.0f);
        std::copy (samples.begin(), samples.end(), data.begin());

        fft.performRealOnlyForwardTransform (data.data(), true);

        std::vector<std::complex<float>> result (static_cast<size_t> (fftSize) / 2 + 1);

        for (size_t bin = 0; bin < result.size(); ++bin)
            result[bin] = { data[bin * 2], data[bin * 2 + 1] };

        return result;
    }

    void configurePhaseMatchedEngine (FirmamentEngine& engine, double sampleRate, float bassMonoHz)
    {
        engine.setWidthPercent (100.0f);
        engine.setLowWidthPercent (100.0f); // unity everywhere so the Side path is the pure LR4 sum
        engine.setBassMonoFrequencyHz (bassMonoHz);
        engine.setBassMonoMode (static_cast<int> (FirmamentEngine::BassMonoMode::phaseMatched));
        engine.setOutputDb (0.0f);
        engine.prepare (makeSpec (sampleRate));
    }
}

TEST_CASE ("Phase Matched bass-mono: per-channel magnitude is flat within +/-0.1 dB from 20 Hz to 20 kHz", "[dsp][engine][bassmono][phasematch][v0.3.0]")
{
    constexpr double sampleRate = 48000.0;

    FirmamentEngine engine;
    configurePhaseMatchedEngine (engine, sampleRate, 120.0f);

    // Left-only impulse excites Mid and Side equally: with the Mid path
    // through AP2 and the Side path through the (phase-identical) LR4 sum,
    // the recombined per-channel response must be a pure allpass - flat
    // magnitude, all the energy back in the Left channel.
    const auto [outLeft, outRight] = renderImpulseResponse (engine, 1.0f, 0.0f, fftSize);

    const auto spectrumLeft = TestHelpers::magnitudeSpectrum (outLeft, fftOrder, false);

    const auto binLow = static_cast<int> (std::ceil (20.0 * fftSize / sampleRate));
    const auto binHigh = static_cast<int> (std::floor (20000.0 * fftSize / sampleRate));

    double maxDeviationDb = 0.0;

    for (int bin = binLow; bin <= binHigh; ++bin)
    {
        const auto magnitudeDb = 20.0 * std::log10 (static_cast<double> (spectrumLeft[static_cast<size_t> (bin)]) + 1.0e-30);
        maxDeviationDb = std::max (maxDeviationDb, std::abs (magnitudeDb));
    }

    CAPTURE (maxDeviationDb);
    CHECK (maxDeviationDb < 0.1);

    // No inter-channel bleed: with the paths phase-matched, nothing decodes
    // into the Right channel.
    float rightPeak = 0.0f;
    for (const auto sample : outRight)
        rightPeak = std::max (rightPeak, std::abs (sample));

    CHECK (rightPeak < 3.1623e-5f); // < -90 dBFS
}

TEST_CASE ("Phase Matched bass-mono: Side-path and Mid-path phases track within 1 degree at every bin; unity negative control fails", "[dsp][engine][bassmono][phasematch][v0.3.0]")
{
    constexpr double sampleRate = 48000.0;
    constexpr float bassMonoHz = 120.0f;

    const auto measureMaxPhaseDifferenceDegrees = [&] (bool bypassMidAllpass)
    {
        // Mid-path response: mono impulse (Side == 0) -> output = Mid path.
        FirmamentEngine midEngine;
        configurePhaseMatchedEngine (midEngine, sampleRate, bassMonoHz);
        midEngine.setPhaseMatchBypassedForTests (bypassMidAllpass);
        midEngine.prepare (makeSpec (sampleRate)); // settle the (test-only) weight change
        const auto midResponse = renderImpulseResponse (midEngine, 0.5f, 0.5f, fftSize).first;

        // Side-path response: anti-phase impulse (Mid == 0) -> Left output =
        // Side path (the LR4 low+high sum at unity widths).
        FirmamentEngine sideEngine;
        configurePhaseMatchedEngine (sideEngine, sampleRate, bassMonoHz);
        sideEngine.setPhaseMatchBypassedForTests (bypassMidAllpass);
        sideEngine.prepare (makeSpec (sampleRate));
        const auto sideResponse = renderImpulseResponse (sideEngine, 0.5f, -0.5f, fftSize).first;

        const auto midSpectrum = complexSpectrum (midResponse);
        const auto sideSpectrum = complexSpectrum (sideResponse);

        const auto binLow = static_cast<int> (std::ceil (20.0 * fftSize / sampleRate));
        const auto binHigh = static_cast<int> (std::floor (20000.0 * fftSize / sampleRate));

        double maxDifferenceDegrees = 0.0;

        for (int bin = binLow; bin <= binHigh; ++bin)
        {
            const auto mid = midSpectrum[static_cast<size_t> (bin)];
            const auto side = sideSpectrum[static_cast<size_t> (bin)];

            // Both paths are allpasses (magnitude ~1); the phase of the
            // cross-spectrum side * conj(mid) is exactly the inter-path
            // phase difference, immune to wrapping.
            const auto cross = side * std::conj (mid);
            const auto differenceDegrees = std::abs (std::arg (cross)) * 180.0 / juce::MathConstants<double>::pi;
            maxDifferenceDegrees = std::max (maxDifferenceDegrees, differenceDegrees);
        }

        return maxDifferenceDegrees;
    };

    const auto matchedDifference = measureMaxPhaseDifferenceDegrees (false);
    CAPTURE (matchedDifference);
    CHECK (matchedDifference <= 1.0);

    // Negative control (brief 6.2): with the Mid companion allpass replaced
    // by unity, the identical assertion must fail - guarding against a
    // silent regression that quietly bypasses the AP2.
    const auto unityControlDifference = measureMaxPhaseDifferenceDegrees (true);
    CAPTURE (unityControlDifference);
    CHECK (unityControlDifference > 1.0);
}

// ===========================================================================
// Linear Phase mode (brief 6.3). Determinism protocol (binding, brief
// 6.3/6.4): after selecting Linear Phase, pump the message loop / invoke the
// message-thread service hop, poll kernelEpoch() with a bounded timeout, and
// discard a settling preroll before opening the measurement window.
namespace
{
    // Prepares an engine in Linear Phase mode and runs the determinism
    // protocol until the initial kernel is verifiably active. Returns false
    // on timeout.
    bool settleLinearPhase (FirmamentEngine& engine, int blockSize = 512)
    {
        juce::AudioBuffer<float> silence (2, blockSize);

        const auto processSilence = [&]
        {
            silence.clear();
            juce::dsp::AudioBlock<float> block (silence);
            engine.process (block);
        };

        const auto ok = TestHelpers::waitForKernelEpoch ([&] { return engine.getLinearPhaseKernelEpoch(); },
                                                         1,
                                                         [&] { engine.serviceLinearPhaseUpdates (true); },
                                                         processSilence);

        // Settling preroll (>= 2 convolution partitions + the loader
        // crossfade): a generous 24000 samples of silence.
        for (int i = 0; i < 48; ++i)
            processSilence();

        return ok;
    }

    void configureLinearPhaseEngine (FirmamentEngine& engine, double sampleRate, float bassMonoHz, float lowWidthPercent)
    {
        engine.setWidthPercent (100.0f);
        engine.setLowWidthPercent (lowWidthPercent);
        engine.setBassMonoFrequencyHz (bassMonoHz);
        engine.setBassMonoMode (static_cast<int> (FirmamentEngine::BassMonoMode::linearPhase));
        engine.setOutputDb (0.0f);
        engine.prepare (makeSpec (sampleRate));
    }
}

TEST_CASE ("Linear Phase bass-mono: at unity widths the output is the input delayed by exactly N/2 samples (perfect reconstruction)", "[dsp][engine][bassmono][linearphase][v0.3.0]")
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    FirmamentEngine engine;
    configureLinearPhaseEngine (engine, sampleRate, 120.0f, 100.0f);
    REQUIRE (settleLinearPhase (engine));

    const auto latency = engine.getLatencySamples();
    CHECK (latency == 2048); // N/2 with N = 4096 @48 kHz

    // Deterministic stereo noise; render enough to compare a full window
    // beyond the delay.
    constexpr int renderLength = 1 << 16;
    TestHelpers::DeterministicPinkNoise pinkLeft (1111u), pinkRight (2222u);

    std::vector<float> inLeft, inRight, outLeft, outRight;
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int rendered = 0; rendered < renderLength; rendered += blockSize)
    {
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < blockSize; ++i)
        {
            left[i] = 0.5f * pinkLeft.nextSample();
            right[i] = 0.5f * pinkRight.nextSample();
            inLeft.push_back (left[i]);
            inRight.push_back (right[i]);
        }

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        outLeft.insert (outLeft.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        outRight.insert (outRight.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + blockSize);
    }

    float peakResidual = 0.0f;

    for (int i = latency; i < renderLength; ++i)
    {
        peakResidual = std::max (peakResidual, std::abs (outLeft[static_cast<size_t> (i)] - inLeft[static_cast<size_t> (i - latency)]));
        peakResidual = std::max (peakResidual, std::abs (outRight[static_cast<size_t> (i)] - inRight[static_cast<size_t> (i - latency)]));
    }

    CAPTURE (peakResidual);
    CHECK (peakResidual < 1.0e-6f); // -120 dBFS
}

TEST_CASE ("Linear Phase bass-mono: Side energy below fc/2 is attenuated by at least 40 dB with Low Width 0", "[dsp][engine][bassmono][linearphase][v0.3.0]")
{
    constexpr double sampleRate = 48000.0;
    constexpr float bassMonoHz = 120.0f;
    constexpr int blockSize = 512;

    FirmamentEngine engine;
    configureLinearPhaseEngine (engine, sampleRate, bassMonoHz, 0.0f);
    REQUIRE (settleLinearPhase (engine));

    // Anti-phase noise: pure Side content. Collect the last fftSize output
    // samples (fully settled) and compare low-frequency energy in/out.
    constexpr int renderLength = fftSize * 2;
    TestHelpers::DeterministicPinkNoise pink (3333u);

    std::vector<float> input, output;
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int rendered = 0; rendered < renderLength; rendered += blockSize)
    {
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < blockSize; ++i)
        {
            const auto sample = 0.5f * pink.nextSample();
            left[i] = sample;
            right[i] = -sample;
            input.push_back (sample);
        }

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);
        output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
    }

    const std::vector<float> inputTail (input.end() - fftSize, input.end());
    const std::vector<float> outputTail (output.end() - fftSize, output.end());

    const auto inputSpectrum = TestHelpers::magnitudeSpectrum (inputTail, fftOrder);
    const auto outputSpectrum = TestHelpers::magnitudeSpectrum (outputTail, fftOrder);

    const auto binLow = juce::jmax (1, static_cast<int> (std::ceil (20.0 * fftSize / sampleRate)));
    const auto binHigh = static_cast<int> (std::floor (0.5 * bassMonoHz * fftSize / sampleRate));

    double inputPower = 0.0, outputPower = 0.0;

    for (int bin = binLow; bin <= binHigh; ++bin)
    {
        inputPower += juce::square (static_cast<double> (inputSpectrum[static_cast<size_t> (bin)]));
        outputPower += juce::square (static_cast<double> (outputSpectrum[static_cast<size_t> (bin)]));
    }

    const auto attenuationDb = 10.0 * std::log10 (inputPower / (outputPower + 1.0e-30));
    CAPTURE (attenuationDb);
    CHECK (attenuationDb >= 40.0);
}

TEST_CASE ("Linear Phase bass-mono: group delay is constant within +/-1 sample across 20 Hz - 20 kHz", "[dsp][engine][bassmono][linearphase][v0.3.0]")
{
    constexpr double sampleRate = 48000.0;

    FirmamentEngine engine;
    configureLinearPhaseEngine (engine, sampleRate, 120.0f, 100.0f);
    REQUIRE (settleLinearPhase (engine));

    const auto latency = engine.getLatencySamples();

    // Full-chain impulse response at unity widths (Left-only impulse
    // excites both paths); group delay from the phase slope per bin pair.
    const auto impulseResponse = renderImpulseResponse (engine, 1.0f, 0.0f, fftSize).first;
    const auto spectrum = complexSpectrum (impulseResponse);

    const auto binLow = juce::jmax (2, static_cast<int> (std::ceil (20.0 * fftSize / sampleRate)));
    const auto binHigh = static_cast<int> (std::floor (20000.0 * fftSize / sampleRate));

    double maxGroupDelayError = 0.0;

    for (int bin = binLow; bin < binHigh; ++bin)
    {
        const auto here = spectrum[static_cast<size_t> (bin)];
        const auto next = spectrum[static_cast<size_t> (bin) + 1];

        // Group delay in samples between adjacent bins: the phase of
        // here * conj(next) divided by the bin spacing in normalised
        // angular frequency - wrap-immune via the cross-spectrum trick.
        const auto phaseStep = std::arg (here * std::conj (next));
        const auto groupDelaySamples = phaseStep * static_cast<double> (fftSize) / juce::MathConstants<double>::twoPi;
        maxGroupDelayError = std::max (maxGroupDelayError, std::abs (groupDelaySamples - latency));
    }

    CAPTURE (maxGroupDelayError, latency);
    CHECK (maxGroupDelayError <= 1.0);
}
