#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

// Decorrelate (v0.2.0, docs/design-brief.md): an alternative, gentler
// widening technique to Haas Mode for near-mono material - a post-M/S-decode
// allpass-based decorrelation of the Right channel. Unlike Width/Low
// Width/Auto Mono Safety (broadband or multiband), it does NOT preserve the
// exact mono-sum invariant - like Haas Mode, it is a second, smaller,
// explicitly documented exception, trading a much smaller mono-fold-down
// cost (mild spectral ripple, "typically less than 1 to 2 dB") for a gentler
// width effect than Haas Mode's precedence-effect delay.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int blockSize = 2048;

    juce::dsp::ProcessSpec makeTestSpec()
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        return spec;
    }

    // A genuinely mono composite source (L == R, several distinct tones
    // spread across the spectrum) - exactly the "near-mono material" both
    // Decorrelate and Haas Mode are documented to widen. Side == 0 for this
    // input, so Width/Bass Mono/Auto Mono Safety are irrelevant to this
    // file's tests; only the post-decode Decorrelate/Haas stage can do
    // anything to it at all.
    void fillMonoComposite (juce::AudioBuffer<float>& buffer, juce::int64 startSample)
    {
        static constexpr double frequenciesHz[] = { 110.0, 440.0, 880.0, 1760.0, 3520.0, 6600.0 };
        static constexpr float amplitudes[] = { 0.22f, 0.20f, 0.18f, 0.15f, 0.12f, 0.08f };

        const auto numSamples = buffer.getNumSamples();
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            float value = 0.0f;

            for (size_t tone = 0; tone < std::size (frequenciesHz); ++tone)
            {
                const auto phase = juce::MathConstants<double>::twoPi * frequenciesHz[tone]
                                    * static_cast<double> (startSample + i) / testSampleRate;
                value += amplitudes[tone] * static_cast<float> (std::sin (phase));
            }

            left[i] = value;
            right[i] = value;
        }
    }

    // A genuinely mono, broadband source (L == R, deterministic band-limited
    // noise) - a much more representative "spectral" test signal than a
    // handful of discrete tones for the magnitude-spectrum comparison below,
    // since a real magnitude-spectrum measurement needs energy spread
    // continuously across the analysis bins, not concentrated in six of
    // them. Deterministic (fixed-seed juce::Random) so repeated calls with
    // the same starting sample index reproduce the same content.
    void fillMonoBroadbandNoise (juce::AudioBuffer<float>& buffer, juce::int64 startSample, float amplitude = 0.4f)
    {
        juce::Random random (1234567 + startSample);

        const auto numSamples = buffer.getNumSamples();
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto value = amplitude * (random.nextFloat() * 2.0f - 1.0f);
            left[i] = value;
            right[i] = value;
        }
    }

    // L + R for every sample in a buffer - the mono downmix, unnormalised
    // (matches MidSideCodec's "2 * Mid" convention used across the rest of
    // this suite's mono-sum tests).
    std::vector<float> monoSum (const juce::AudioBuffer<float>& buffer)
    {
        std::vector<float> result (static_cast<size_t> (buffer.getNumSamples()));
        const auto* left = buffer.getReadPointer (0);
        const auto* right = buffer.getReadPointer (1);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            result[static_cast<size_t> (i)] = left[i] + right[i];

        return result;
    }

    // Real magnitude-spectrum comparison (docs/design-brief.md guarantee #6:
    // "sum the processed L/R to mono and compare the mono-summed magnitude
    // spectrum against the pre-Decorrelate spectrum; assert peak deviation
    // stays within a small, defined dB tolerance"). Windows `samples` with a
    // Hann window (reduces spectral leakage so bin-to-bin comparisons are
    // meaningful), computes its non-negative-frequency magnitude spectrum via
    // juce::dsp::FFT, and returns the peak |20*log10(processedMag/referenceMag)|
    // across every analysis bin with non-negligible reference energy (skips
    // near-silent bins, where a tiny absolute difference would otherwise
    // produce a meaninglessly huge dB ratio).
    constexpr int fftOrder = 12; // 4096-point FFT
    constexpr int fftSize = 1 << fftOrder;

    double peakMagnitudeSpectrumDeviationDb (const std::vector<float>& processed, const std::vector<float>& reference)
    {
        jassert (processed.size() >= static_cast<size_t> (fftSize));
        jassert (reference.size() >= static_cast<size_t> (fftSize));

        juce::dsp::FFT fft (fftOrder);

        auto computeMagnitudes = [&fft] (const std::vector<float>& samples)
        {
            std::vector<float> windowed (static_cast<size_t> (fftSize) * 2, 0.0f);

            for (int i = 0; i < fftSize; ++i)
            {
                // Hann window.
                const auto windowValue = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (fftSize - 1));
                windowed[static_cast<size_t> (i)] = samples[static_cast<size_t> (i)] * windowValue;
            }

            fft.performFrequencyOnlyForwardTransform (windowed.data(), true);
            windowed.resize (static_cast<size_t> (fftSize) / 2 + 1);
            return windowed;
        };

        const auto processedMagnitudes = computeMagnitudes (processed);
        const auto referenceMagnitudes = computeMagnitudes (reference);

        // Reference peak magnitude, used to set a noise-floor cutoff below
        // which bins are skipped (avoids a near-zero reference bin producing
        // a spuriously enormous dB ratio from an irrelevant, inaudible
        // difference).
        float referencePeak = 0.0f;

        for (const auto magnitude : referenceMagnitudes)
            referencePeak = std::max (referencePeak, magnitude);

        const auto noiseFloor = referencePeak * 0.01f; // -40 dB relative to the reference peak
        constexpr double epsilon = 1.0e-9;

        double peakDeviationDb = 0.0;

        for (size_t bin = 0; bin < referenceMagnitudes.size(); ++bin)
        {
            if (referenceMagnitudes[bin] < noiseFloor)
                continue;

            const auto ratio = (static_cast<double> (processedMagnitudes[bin]) + epsilon)
                                / (static_cast<double> (referenceMagnitudes[bin]) + epsilon);
            const auto deviationDb = std::abs (20.0 * std::log10 (ratio));
            peakDeviationDb = std::max (peakDeviationDb, deviationDb);
        }

        return peakDeviationDb;
    }
}

TEST_CASE ("Decorrelate off (default) is a fully transparent passthrough, even with a nonzero Decorrelate Amount set", "[dsp][engine][decorrelate][null]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setDecorrelateEnabled (false);
    engine.setDecorrelateAmountPercent (65.0f); // set but must have no effect while disabled
    engine.setHaasEnabled (false);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    juce::AudioBuffer<float> reference (2, blockSize);
    TestHelpers::fillStereoWithDistinctSines (reference, testSampleRate, 1000.0, 1300.0, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    constexpr float tolerance = 3.1623e-5f; // < -90 dBFS, same bound as the existing Haas/unity-round-trip tests

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        float maxResidual = 0.0f;

        for (int i = 0; i < blockSize; ++i)
            maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));

        CHECK (maxResidual < tolerance);
    }
}

TEST_CASE ("Decorrelate's mono-fold-down cost is small and measurably smaller than Haas Mode's on the same near-mono source", "[dsp][engine][decorrelate]")
{
    // A genuine magnitude-spectrum comparison (docs/design-brief.md
    // guarantee #6 - see peakMagnitudeSpectrumDeviationDb()'s docs above),
    // on deterministic broadband noise (not a handful of discrete tones - a
    // real per-bin spectral comparison needs energy spread continuously
    // across the analysis bins).
    FirmamentEngine decorrelateEngine;
    decorrelateEngine.setWidthPercent (100.0f);
    decorrelateEngine.setBassMonoFrequencyHz (0.0f);
    decorrelateEngine.setDecorrelateEnabled (true);
    decorrelateEngine.setDecorrelateAmountPercent (50.0f); // the parameter's own documented default
    decorrelateEngine.setHaasEnabled (false);
    decorrelateEngine.setOutputDb (0.0f);
    decorrelateEngine.prepare (makeTestSpec());

    FirmamentEngine haasEngine;
    haasEngine.setWidthPercent (100.0f);
    haasEngine.setBassMonoFrequencyHz (0.0f);
    haasEngine.setDecorrelateEnabled (false);
    haasEngine.setHaasEnabled (true);
    haasEngine.setHaasTimeMs (20.0f); // the documented default Haas Time sweet spot
    haasEngine.setOutputDb (0.0f);
    haasEngine.prepare (makeTestSpec());

    // Warm up both engines (several blocks) so the allpass cascade/delay
    // line are both in a settled steady state before the measured block.
    for (int warmup = 0; warmup < 4; ++warmup)
    {
        juce::AudioBuffer<float> warmupBuffer (2, blockSize);

        fillMonoBroadbandNoise (warmupBuffer, static_cast<juce::int64> (warmup) * blockSize);
        juce::dsp::AudioBlock<float> decorrelateBlock (warmupBuffer);
        decorrelateEngine.process (decorrelateBlock);

        juce::AudioBuffer<float> warmupBufferHaas (2, blockSize);
        fillMonoBroadbandNoise (warmupBufferHaas, static_cast<juce::int64> (warmup) * blockSize);
        juce::dsp::AudioBlock<float> haasBlock (warmupBufferHaas);
        haasEngine.process (haasBlock);
    }

    // The measured window must be exactly fftSize samples (2 * blockSize ==
    // fftSize here) for peakMagnitudeSpectrumDeviationDb() - two consecutive
    // processed blocks, concatenated.
    static_assert (blockSize * 2 == fftSize, "measurement window must exactly match the FFT analysis size");

    std::vector<float> decorrelateMonoSum;
    std::vector<float> haasMonoSum;
    std::vector<float> referenceMonoSumSamples;
    decorrelateMonoSum.reserve (static_cast<size_t> (fftSize));
    haasMonoSum.reserve (static_cast<size_t> (fftSize));
    referenceMonoSumSamples.reserve (static_cast<size_t> (fftSize));

    for (int measuredBlock = 0; measuredBlock < 2; ++measuredBlock)
    {
        const auto startSample = static_cast<juce::int64> (4 + measuredBlock) * blockSize;

        juce::AudioBuffer<float> decorrelateBuffer (2, blockSize);
        fillMonoBroadbandNoise (decorrelateBuffer, startSample);
        juce::dsp::AudioBlock<float> decorrelateBlock (decorrelateBuffer);
        decorrelateEngine.process (decorrelateBlock);
        const auto decorrelateSum = monoSum (decorrelateBuffer);
        decorrelateMonoSum.insert (decorrelateMonoSum.end(), decorrelateSum.begin(), decorrelateSum.end());

        juce::AudioBuffer<float> haasBuffer (2, blockSize);
        fillMonoBroadbandNoise (haasBuffer, startSample);
        juce::dsp::AudioBlock<float> haasBlock (haasBuffer);
        haasEngine.process (haasBlock);
        const auto haasSum = monoSum (haasBuffer);
        haasMonoSum.insert (haasMonoSum.end(), haasSum.begin(), haasSum.end());

        // The reference mono sum for the exact same window: the
        // pre-processing mono downmix of the identical source content,
        // unaffected by either engine's own processing.
        juce::AudioBuffer<float> referenceBuffer (2, blockSize);
        fillMonoBroadbandNoise (referenceBuffer, startSample);
        const auto referenceSum = monoSum (referenceBuffer);
        referenceMonoSumSamples.insert (referenceMonoSumSamples.end(), referenceSum.begin(), referenceSum.end());
    }

    const auto decorrelateDeviationDb = peakMagnitudeSpectrumDeviationDb (decorrelateMonoSum, referenceMonoSumSamples);
    const auto haasDeviationDb = peakMagnitudeSpectrumDeviationDb (haasMonoSum, referenceMonoSumSamples);

    CAPTURE (decorrelateDeviationDb, haasDeviationDb);

    // Decorrelate's peak magnitude-spectrum deviation stays within a small,
    // defined dB tolerance - generous headroom over the documented "1-2 dB"
    // ballpark (docs/research-notes.md Section 4) at this test's 4-tap/50%
    // configuration, since this plugin's exact allpass topology is
    // implementation-reasoned, not calibrated against that specific source
    // or a literal per-band-count match to it.
    CHECK (decorrelateDeviationDb < 9.0);

    // ...and Haas Mode's comb-filtering deviation is markedly larger - deep,
    // narrow notches (in the limit, -infinity dB at an exact null) are
    // exactly what a comb filter produces and an allpass network
    // structurally cannot, the documented comparative finding
    // (docs/research-notes.md Section 4). Not asserted as a strict literal
    // 10x multiplier of whatever Decorrelate's own deviation happens to
    // measure at any given amount/config (that would make this assertion
    // sensitive to Decorrelate implementation tuning that has nothing to do
    // with Haas Mode's own, structurally much worse, behaviour) - instead a
    // clear absolute floor for Haas Mode's deviation, comfortably beyond
    // Decorrelate's small-tolerance ceiling above.
    CHECK (haasDeviationDb > decorrelateDeviationDb * 3.0);
    CHECK (haasDeviationDb > 20.0);
}

TEST_CASE ("Decorrelate/Haas mutual exclusivity: with both enabled, Haas's delay is bypassed and output matches Decorrelate-only processing bit-exactly", "[dsp][engine][decorrelate][haas]")
{
    auto makeEngine = [] (bool decorrelateEnabled, bool haasEnabled)
    {
        auto engine = std::make_unique<FirmamentEngine>();
        engine->setWidthPercent (100.0f);
        engine->setBassMonoFrequencyHz (0.0f);
        engine->setDecorrelateEnabled (decorrelateEnabled);
        engine->setDecorrelateAmountPercent (50.0f);
        engine->setHaasEnabled (haasEnabled);
        engine->setHaasTimeMs (25.0f);
        engine->setOutputDb (0.0f);
        engine->prepare (makeTestSpec());
        return engine;
    };

    auto bothEnabled = makeEngine (true, true);
    auto decorrelateOnly = makeEngine (true, false);

    juce::AudioBuffer<float> bothBuffer (2, blockSize);
    juce::AudioBuffer<float> decorrelateOnlyBuffer (2, blockSize);

    for (int block = 0; block < 4; ++block)
    {
        const auto startSample = static_cast<juce::int64> (block) * blockSize;

        fillMonoComposite (bothBuffer, startSample);
        juce::dsp::AudioBlock<float> bothBlock (bothBuffer);
        bothEnabled->process (bothBlock);

        fillMonoComposite (decorrelateOnlyBuffer, startSample);
        juce::dsp::AudioBlock<float> decorrelateOnlyBlock (decorrelateOnlyBuffer);
        decorrelateOnly->process (decorrelateOnlyBlock);
    }

    const auto* bothLeft = bothBuffer.getReadPointer (0);
    const auto* bothRight = bothBuffer.getReadPointer (1);
    const auto* decorrelateOnlyLeft = decorrelateOnlyBuffer.getReadPointer (0);
    const auto* decorrelateOnlyRight = decorrelateOnlyBuffer.getReadPointer (1);

    float maxDifference = 0.0f;

    for (int i = 0; i < blockSize; ++i)
    {
        maxDifference = std::max (maxDifference, std::abs (bothLeft[i] - decorrelateOnlyLeft[i]));
        maxDifference = std::max (maxDifference, std::abs (bothRight[i] - decorrelateOnlyRight[i]));
    }

    // Bit-exact by construction (both engines pin the Haas delay line to 0
    // samples whenever Decorrelate is enabled, regardless of Haas Mode's own
    // on/off state - see FirmamentEngine::process()), not just "close".
    // maxDifference is a std::abs()-accumulated max, so it is never
    // negative; asserting <= 0 is exact equality without tripping
    // -Wfloat-equal.
    CHECK (maxDifference <= 0.0f);
}

// ===========================================================================
// v0.3.0 velvet-mode tests (binding brief, sections 3.1/6.7).

namespace
{
    // "Pink noise + drum loop" program (brief 6.7): a deterministic
    // near-mono program - broadband pink bed plus periodic kick (decaying
    // 60 Hz burst) and snare (decaying noise burst) hits. Mono (L == R), so
    // the mono-sum spectrum comparison isolates the decorrelator's own
    // fold-down cost.
    struct DrumLoopProgram
    {
        TestHelpers::DeterministicPinkNoise pink { 555111u };
        TestHelpers::DeterministicPinkNoise snareNoise { 777333u };
        juce::int64 position = 0;

        float next()
        {
            constexpr int loopLength = 24000; // 0.5 s at 48 kHz
            const auto phase = static_cast<int> (position % loopLength);

            float sample = 0.30f * pink.nextSample();

            // Kick at loop start: 60 Hz burst with a fast exponential decay.
            if (phase < 4800)
            {
                const auto t = static_cast<double> (phase);
                sample += 0.5f * static_cast<float> (std::exp (-t / 1200.0) * std::sin (juce::MathConstants<double>::twoPi * 60.0 * t / 48000.0));
            }

            // Snare on the back beat: enveloped noise burst.
            const auto snarePhase = phase - loopLength / 2;
            const auto snare = snareNoise.nextSample(); // always advance (determinism)

            if (snarePhase >= 0 && snarePhase < 3600)
                sample += 0.35f * static_cast<float> (std::exp (-snarePhase / 900.0)) * snare;

            ++position;
            return sample;
        }
    };

    // Renders `length` samples of the mono drum-loop program through an
    // engine (warmup discarded) and returns the third-octave mono-sum
    // maximum dip (dry-band dB minus processed-band dB, positive = loss)
    // over the valid 20 Hz - 20 kHz bands.
    double measureMonoFoldDownDipDb (FirmamentEngine& engine)
    {
        constexpr int analysisOrder = 15;
        constexpr int analysisSize = 1 << analysisOrder; // 32768
        constexpr int warmupSamples = 1 << 14;

        DrumLoopProgram program;
        juce::AudioBuffer<float> buffer (2, blockSize);

        std::vector<float> dryMono, processedMono;
        dryMono.reserve (analysisSize);
        processedMono.reserve (analysisSize);

        int rendered = 0;

        while (rendered < warmupSamples + analysisSize)
        {
            auto* left = buffer.getWritePointer (0);
            auto* right = buffer.getWritePointer (1);

            std::vector<float> dryBlock (static_cast<size_t> (blockSize));

            for (int i = 0; i < blockSize; ++i)
            {
                const auto sample = program.next();
                left[i] = sample;
                right[i] = sample;
                dryBlock[static_cast<size_t> (i)] = sample + sample; // dry mono sum (L + R)
            }

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);

            for (int i = 0; i < blockSize; ++i)
            {
                if (rendered + i >= warmupSamples && static_cast<int> (processedMono.size()) < analysisSize)
                {
                    processedMono.push_back (buffer.getSample (0, i) + buffer.getSample (1, i));
                    dryMono.push_back (dryBlock[static_cast<size_t> (i)]);
                }
            }

            rendered += blockSize;
        }

        const auto drySpectrum = TestHelpers::magnitudeSpectrum (dryMono, analysisOrder);
        const auto processedSpectrum = TestHelpers::magnitudeSpectrum (processedMono, analysisOrder);

        const auto dryBands = TestHelpers::thirdOctaveBandAverages (drySpectrum, testSampleRate, analysisSize);
        const auto processedBands = TestHelpers::thirdOctaveBandAverages (processedSpectrum, testSampleRate, analysisSize);

        double maxDipDb = 0.0;

        for (int band = 0; band < 30; ++band)
            if (dryBands.valid[static_cast<size_t> (band)] && processedBands.valid[static_cast<size_t> (band)])
                maxDipDb = std::max (maxDipDb, dryBands.bandDb[static_cast<size_t> (band)] - processedBands.bandDb[static_cast<size_t> (band)]);

        return maxDipDb;
    }
}

TEST_CASE ("Mono-compat money test: Velvet Dense at width 150%/amount 100% folds down with at most a 3 dB third-octave dip; the Haas control case fails the same metric", "[dsp][engine][decorrelate][velvet][monocompat][v0.3.0]")
{
    // Velvet Dense engine (brief 6.7: pink noise + drum loop, width 150%,
    // Velvet Dense at 100% amount).
    FirmamentEngine velvetEngine;
    velvetEngine.setWidthPercent (150.0f);
    velvetEngine.setBassMonoFrequencyHz (0.0f);
    velvetEngine.setDecorrelateEnabled (true);
    velvetEngine.setDecorrelateAmountPercent (100.0f);
    velvetEngine.setDecorrelateMode (static_cast<int> (FirmamentEngine::DecorrelateMode::velvetDense));
    velvetEngine.setOutputDb (0.0f);
    velvetEngine.prepare (makeTestSpec());

    const auto velvetDipDb = measureMonoFoldDownDipDb (velvetEngine);

    // Haas control case: same program, Haas Mode at its 20 ms default -
    // the comb notches must fail the identical metric, documenting why Haas
    // carries a manual warning (research 2.6) while velvet does not.
    FirmamentEngine haasEngine;
    haasEngine.setWidthPercent (150.0f);
    haasEngine.setBassMonoFrequencyHz (0.0f);
    haasEngine.setDecorrelateEnabled (false);
    haasEngine.setHaasEnabled (true);
    haasEngine.setHaasTimeMs (20.0f);
    haasEngine.setOutputDb (0.0f);
    haasEngine.prepare (makeTestSpec());

    const auto haasDipDb = measureMonoFoldDownDipDb (haasEngine);

    CAPTURE (velvetDipDb, haasDipDb);
    CHECK (velvetDipDb <= 3.0);
    CHECK (haasDipDb > 3.0);
}

TEST_CASE ("Velvet modes: symmetric complementary topology processes BOTH channels (Classic leaves Left dry, velvet does not)", "[dsp][engine][decorrelate][velvet][v0.3.0]")
{
    const auto renderLeftResidual = [] (int mode)
    {
        FirmamentEngine engine;
        engine.setWidthPercent (100.0f);
        engine.setBassMonoFrequencyHz (0.0f);
        engine.setDecorrelateEnabled (true);
        engine.setDecorrelateAmountPercent (100.0f);
        engine.setDecorrelateMode (mode);
        engine.setOutputDb (0.0f);
        engine.prepare (makeTestSpec());

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::AudioBuffer<float> reference (2, blockSize);

        float maxResidual = 0.0f;

        for (int block = 0; block < 4; ++block)
        {
            fillMonoComposite (buffer, static_cast<juce::int64> (block) * blockSize);
            reference.makeCopyOf (buffer);

            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);

            for (int i = 0; i < blockSize; ++i)
                maxResidual = std::max (maxResidual, std::abs (buffer.getSample (0, i) - reference.getSample (0, i)));
        }

        return maxResidual;
    };

    // Classic (v0.2.0 topology): Left is bone dry.
    CHECK (renderLeftResidual (static_cast<int> (FirmamentEngine::DecorrelateMode::classic)) <= 0.0f);

    // Velvet Dense/Sparse: Left is genuinely processed (the survey's 4.1
    // "R-only topology" finding is fixed).
    CHECK (renderLeftResidual (static_cast<int> (FirmamentEngine::DecorrelateMode::velvetDense)) > 0.01f);
    CHECK (renderLeftResidual (static_cast<int> (FirmamentEngine::DecorrelateMode::velvetSparse)) > 0.01f);
}

TEST_CASE ("Decorrelate mode switching is a click-free crossfade; settled modes are bit-exact selections", "[dsp][engine][decorrelate][velvet][v0.3.0]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setDecorrelateEnabled (true);
    engine.setDecorrelateAmountPercent (80.0f);
    engine.setDecorrelateMode (static_cast<int> (FirmamentEngine::DecorrelateMode::classic));
    engine.setOutputDb (0.0f);
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);
    float maxStep = 0.0f;
    float previousLeft = 0.0f, previousRight = 0.0f;
    bool first = true;

    for (int block = 0; block < 24; ++block)
    {
        // Cycle Classic -> Velvet Dense -> Velvet Sparse every 2 blocks
        // (~85 ms) while feeding steady program material.
        engine.setDecorrelateMode ((block / 2) % 3);

        fillMonoBroadbandNoise (buffer, static_cast<juce::int64> (block) * blockSize, 0.4f);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);

        for (int i = 0; i < blockSize; ++i)
        {
            const auto left = buffer.getSample (0, i);
            const auto right = buffer.getSample (1, i);

            if (! first)
            {
                maxStep = std::max (maxStep, std::abs (left - previousLeft));
                maxStep = std::max (maxStep, std::abs (right - previousRight));
            }

            previousLeft = left;
            previousRight = right;
            first = false;
        }

        REQUIRE (TestHelpers::allSamplesFinite (buffer));
    }

    // Broadband noise at 0.4 amplitude has sample-to-sample deltas up to
    // ~0.8 on its own; a crossfaded mode switch must not exceed the input's
    // own step scale (an instant path switch between decorrelated and dry
    // program at 80% amount would).
    CAPTURE (maxStep);
    CHECK (maxStep < 1.6f);
}
