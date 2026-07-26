#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

// Firmament is a purely linear signal path (Mid/Side scale + a zero-latency
// crossover + an output trim) with no saturating/nonlinear stage anywhere in
// it, unlike e.g. Overture's clipper. That means it has no built-in ceiling:
// a single pass at maximum Width (200%) and maximum Output (+24 dB) can and
// should produce a louder-than-unity result for full-scale input - that is
// the parameter doing exactly what it is documented to do, not a bug.
// These tests document the actual, bounded behaviour of a single realistic
// processBlock() call (a host always supplies a fresh block of audio on
// every callback, never re-feeds a plugin's own prior output back in as new
// input), as a sane-headroom regression test distinct from the NaN/Inf
// sweep in RobustnessTests.cpp.
TEST_CASE ("Single-pass full-scale input at maximum Width/Output stays within sane, finite headroom", "[dsp][engine][gainstaging]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (200.0f);
    engine.setBassMonoFrequencyHz (500.0f);
    engine.setOutputDb (24.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 2;
    engine.prepare (spec);

    for (int block = 0; block < 8; ++block)
    {
        // A *fresh* full-scale signal every block, as a real host would
        // supply - not the previous block's already-amplified output.
        juce::AudioBuffer<float> buffer (2, 512);
        TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0, 1000.0, 1300.0, 1.0f);

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);

        CHECK (TestHelpers::allSamplesFinite (buffer));

        // Worst case: Side scaled to 2x plus a fully-additive Mid/Side
        // decode (up to 2x the encoded amplitude) at +24 dB (~15.85x) of a
        // 1.0-amplitude input - comfortably under 100 with margin for the
        // two overlapping test tones' constructive interference.
        CHECK (TestHelpers::peakAbsolute (buffer) < 100.0f);
    }
}

// ===========================================================================
// v0.3.0 equal-power width compensation (binding brief, sections 3.5/6.13).

namespace
{
    // A deterministic, *exactly* decorrelated pink stereo pair for the
    // equal-power law: L is built from the odd harmonics of a shared
    // 8192-sample period, R from the even harmonics, amplitudes ~ 1/sqrt(k)
    // (pink power spectrum), phases from a fixed LCG. Distinct sinusoid
    // frequencies are orthogonal over any whole number of periods, so the
    // sample cross-correlation over the measurement window (a multiple of
    // the period, see below) is exactly zero - the case the compensation
    // law g = 1/sqrt(a^2 + b^2) is exact for. Two LCG-seeded
    // DeterministicPinkNoise streams are NOT usable here: their slow
    // Voss-McCartney rows hold near-constant offsets across any practical
    // window, which shows up as a spurious inter-channel correlation of
    // ~0.1 and biases the RMS by several tenths of a dB.
    struct OrthogonalPinkPair
    {
        static constexpr int period = 8192;

        std::vector<float> left, right;

        OrthogonalPinkPair()
        {
            std::vector<double> l (period, 0.0), r (period, 0.0);

            juce::uint32 lcg = 20260726u;
            const auto nextPhase = [&lcg]
            {
                lcg = lcg * 1664525u + 1013904223u;
                return static_cast<double> (lcg >> 8) * (juce::MathConstants<double>::twoPi / 16777216.0);
            };

            // Bins 3..800 of the 8192-sample period: ~17.6 Hz .. ~4.7 kHz
            // at 48 kHz, comfortably inside the audio band at every rate
            // this test runs at.
            for (int k = 3; k <= 800; ++k)
            {
                const auto amplitude = 1.0 / std::sqrt (static_cast<double> (k));
                const auto phase = nextPhase();
                auto& destination = (k & 1) != 0 ? l : r;

                for (int n = 0; n < period; ++n)
                    destination[static_cast<size_t> (n)] += amplitude * std::sin (juce::MathConstants<double>::twoPi * k * n / period + phase);
            }

            // Normalise both channels to identical RMS (0.1), so the
            // equal-power law's "equal channel power" premise holds exactly.
            const auto normalise = [] (std::vector<double>& channel)
            {
                double sumOfSquares = 0.0;
                for (const auto value : channel)
                    sumOfSquares += value * value;

                const auto gain = 0.1 / std::sqrt (sumOfSquares / period);

                std::vector<float> result (channel.size());
                for (size_t i = 0; i < channel.size(); ++i)
                    result[i] = static_cast<float> (channel[i] * gain);
                return result;
            };

            left = normalise (l);
            right = normalise (r);
        }

        void fillBlock (juce::AudioBuffer<float>& buffer, juce::int64 startSample) const
        {
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const auto index = static_cast<size_t> ((startSample + i) % period);
                buffer.setSample (0, i, left[index]);
                buffer.setSample (1, i, right[index]);
            }
        }
    };
}

TEST_CASE ("Equal-power width compensation: decorrelated pink noise keeps its RMS within +/-0.5 dB across width 0-200% with widthComp on; off reproduces the uncompensated curve exactly", "[dsp][engine][gainstaging][widthcomp][v0.3.0]")
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 2048;
    constexpr int numBlocks = 24;

    // Shared across every measureRms() call (deterministic; building it
    // once keeps the test fast).
    static const OrthogonalPinkPair program;

    // The measured window below is blocks 4..23 = 20 * 2048 = 40960 samples
    // = exactly 5 periods of the program, so the channels' orthogonality
    // (and each sinusoid's whole number of cycles) holds *exactly* over the
    // window.
    static_assert ((numBlocks - 4) * blockSize % OrthogonalPinkPair::period == 0);

    const auto measureRms = [&] (float widthPercent, bool compensated)
    {
        FirmamentEngine engine;
        engine.setWidthPercent (widthPercent);
        engine.setBassMonoFrequencyHz (0.0f);
        engine.setWidthCompensationEnabled (compensated);
        engine.setOutputDb (0.0f);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        engine.prepare (spec);

        juce::AudioBuffer<float> buffer (2, blockSize);
        double sumOfSquares = 0.0;
        juce::int64 counted = 0;

        for (int block = 0; block < numBlocks; ++block)
        {
            program.fillBlock (buffer, static_cast<juce::int64> (block) * blockSize);

            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);

            if (block >= 4) // skip the 50 ms smoother settling
            {
                for (int channel = 0; channel < 2; ++channel)
                {
                    const auto* data = buffer.getReadPointer (channel);

                    for (int i = 0; i < blockSize; ++i)
                    {
                        sumOfSquares += static_cast<double> (data[i]) * data[i];
                        ++counted;
                    }
                }
            }
        }

        return std::sqrt (sumOfSquares / static_cast<double> (counted));
    };

    const auto referenceRms = measureRms (100.0f, true);

    for (const auto width : { 0.0f, 50.0f, 150.0f, 200.0f })
    {
        const auto compensatedRms = measureRms (width, true);
        const auto deviationDb = 20.0 * std::log10 (compensatedRms / referenceRms);

        CAPTURE (width, deviationDb);
        CHECK (std::abs (deviationDb) <= 0.5);
    }

    // widthComp off must reproduce the current (uncompensated) curve
    // exactly: at width 200% the uncompensated output is measurably hotter,
    // and the theoretical curve sqrt(a^2 + b^2) holds within 0.2 dB.
    for (const auto width : { 0.0f, 200.0f })
    {
        const auto uncompensatedRms = measureRms (width, false);
        const auto w = width * 0.01f;
        const auto a = (1.0f + w) * 0.5f;
        const auto b = (1.0f - w) * 0.5f;
        const auto expectedRatio = std::sqrt (static_cast<double> (a) * a + static_cast<double> (b) * b);
        const auto measuredRatio = uncompensatedRms / measureRms (100.0f, false);

        CAPTURE (width, expectedRatio, measuredRatio);
        CHECK (std::abs (20.0 * std::log10 (measuredRatio / expectedRatio)) < 0.2);
    }

    // And with compensation off, explicitly setting widthComp = false is
    // bit-identical to never touching the parameter (neutral default).
    {
        FirmamentEngine touched, untouched;

        for (auto* engine : { &touched, &untouched })
        {
            engine->setWidthPercent (170.0f);
            engine->setOutputDb (0.0f);
        }

        touched.setWidthCompensationEnabled (false);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        touched.prepare (spec);
        untouched.prepare (spec);

        TestHelpers::DeterministicPinkNoise pinkA (12u), pinkB (34u);
        TestHelpers::DeterministicPinkNoise pinkC (12u), pinkD (34u);

        juce::AudioBuffer<float> bufferTouched (2, blockSize), bufferUntouched (2, blockSize);
        float peakDifference = 0.0f;

        for (int block = 0; block < 8; ++block)
        {
            TestHelpers::fillStereoWithDeterministicPinkNoise (bufferTouched, pinkA, pinkB, 0.35f);
            TestHelpers::fillStereoWithDeterministicPinkNoise (bufferUntouched, pinkC, pinkD, 0.35f);

            juce::dsp::AudioBlock<float> blockTouched (bufferTouched);
            juce::dsp::AudioBlock<float> blockUntouched (bufferUntouched);
            touched.process (blockTouched);
            untouched.process (blockUntouched);

            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < blockSize; ++i)
                    peakDifference = std::max (peakDifference,
                                               std::abs (bufferTouched.getSample (channel, i) - bufferUntouched.getSample (channel, i)));
        }

        CHECK (peakDifference <= 0.0f);
    }
}
