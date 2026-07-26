#include "PluginProcessor.h"
#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <memory>

// Correlation/phase meter (M1): FirmamentEngine::getCorrelationValue() is a
// running, leaky-integrated (200 ms) estimate of the plugin's input L/R
// correlation, in [-1, 1]. It drives Auto Mono Safety internally and is
// exposed for a future GUI meter (M3 scope - see FirmamentAudioProcessor::
// getCorrelationMeterValue()).
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int blockSize = 2048;

    // v0.2.0: bumped from 30 to 45 blocks to keep the same settling margin
    // now that the ballistics time constant moved from 200ms to 300ms (see
    // docs/design-brief.md) - 45 * 2048 / 48000 ~= 1.92 s ~= 6.4 time
    // constants at 300 ms.
    constexpr int settleBlocks = 45;

    juce::dsp::ProcessSpec makeTestSpec()
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        return spec;
    }
}

TEST_CASE ("Correlation estimate approaches +1 for identical (in-phase) L/R", "[dsp][engine][correlation]")
{
    FirmamentEngine engine;
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int block = 0; block < settleBlocks; ++block)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, 500.0, 0.5f, static_cast<juce::int64> (block) * blockSize);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    CHECK (engine.getCorrelationValue() > 0.9f);
}

TEST_CASE ("Correlation estimate approaches -1 for inverted (anti-phase) L/R", "[dsp][engine][correlation]")
{
    FirmamentEngine engine;
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int block = 0; block < settleBlocks; ++block)
    {
        const auto startSample = static_cast<juce::int64> (block) * blockSize;
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < blockSize; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * 500.0 * static_cast<double> (startSample + i) / testSampleRate;
            const auto value = 0.5f * static_cast<float> (std::sin (phase));
            left[i] = value;
            right[i] = -value;
        }

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    CHECK (engine.getCorrelationValue() < -0.9f);
}

TEST_CASE ("Correlation estimate stays finite and near zero for silence", "[dsp][engine][correlation]")
{
    FirmamentEngine engine;
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();

    for (int block = 0; block < 4; ++block)
    {
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    CHECK (std::isfinite (engine.getCorrelationValue()));
    CHECK (std::abs (engine.getCorrelationValue()) < 1.0e-3f);
}

TEST_CASE ("Correlation estimate stays within [-1, 1] and finite across a randomised sweep", "[dsp][engine][correlation]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (150.0f);
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int block = 0; block < 50; ++block)
    {
        const auto leftFreq = 100.0 + 37.0 * block;
        const auto rightFreq = 150.0 + 53.0 * block;
        TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, leftFreq, rightFreq, 0.7f);

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);

        const auto correlation = engine.getCorrelationValue();
        CHECK (std::isfinite (correlation));
        CHECK (correlation >= -1.0f);
        CHECK (correlation <= 1.0f);
    }
}

TEST_CASE ("FirmamentAudioProcessor::getCorrelationMeterValue() reflects the engine after processBlock", "[processor][correlation]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < settleBlocks; ++block)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, 500.0, 0.5f, static_cast<juce::int64> (block) * blockSize);
        processor.processBlock (buffer, midi);
    }

    CHECK (processor.getCorrelationMeterValue() > 0.9f);
}

// ===========================================================================
// v0.3.0 meter surface (binding brief, sections 3.8/6.9): the beis.de trap
// signal, the energy-gate silence decay, the new per-band input meters and
// the output (post-processing) meter.

namespace
{
    void processBlocks (FirmamentEngine& engine, juce::AudioBuffer<float>& buffer, int numBlocks,
                        const std::function<void (juce::AudioBuffer<float>&, juce::int64)>& fill)
    {
        for (int block = 0; block < numBlocks; ++block)
        {
            fill (buffer, static_cast<juce::int64> (block) * buffer.getNumSamples());
            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);
        }
    }
}

TEST_CASE ("Correlation meter: the beis.de trap signal reads the true Pearson value, not 1.0", "[dsp][engine][correlation][v0.3.0]")
{
    // beis.de: L = sin(2 pi 1 kHz), R = 0.3 sin(2 pi 1 kHz) + 0.95 sin(2 pi
    // 3.1 kHz). A zero-crossing/XOR "phase meter" reads ~100% on this; a
    // true correlation meter must read the Pearson value (~0.3).
    FirmamentEngine engine;
    engine.setOutputDb (0.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 2048;
    spec.numChannels = 2;
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, 2048);

    double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;

    const auto fillTrap = [&] (juce::AudioBuffer<float>& target, juce::int64 startSample)
    {
        auto* left = target.getWritePointer (0);
        auto* right = target.getWritePointer (1);

        for (int i = 0; i < target.getNumSamples(); ++i)
        {
            const auto t = static_cast<double> (startSample + i) / 48000.0;
            const auto l = std::sin (juce::MathConstants<double>::twoPi * 1000.0 * t);
            const auto r = 0.3 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * t)
                           + 0.95 * std::sin (juce::MathConstants<double>::twoPi * 3100.0 * t);
            left[i] = 0.5f * static_cast<float> (l);
            right[i] = 0.5f * static_cast<float> (r);

            sumLR += l * r;
            sumLL += l * l;
            sumRR += r * r;
        }
    };

    processBlocks (engine, buffer, 60, fillTrap); // ~2.5 s, well past the 300 ms settle

    const auto truePearson = sumLR / std::sqrt (sumLL * sumRR);
    const auto measured = static_cast<double> (engine.getCorrelationValue());

    CAPTURE (truePearson, measured);
    CHECK (std::abs (measured - truePearson) < 0.02);
    CHECK (measured < 0.9); // and emphatically NOT the phase-meter's ~1.0
}

TEST_CASE ("Correlation meters: energy gate decays every estimate toward 0 under sustained silence (never shows +/-1 on silence)", "[dsp][engine][correlation][v0.3.0]")
{
    FirmamentEngine engine;
    engine.setOutputDb (0.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 2048;
    spec.numChannels = 2;
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, 2048);

    // Drive all meters hard anti-phase first...
    processBlocks (engine, buffer, 48, [] (juce::AudioBuffer<float>& target, juce::int64 startSample)
    {
        auto* left = target.getWritePointer (0);
        auto* right = target.getWritePointer (1);

        for (int i = 0; i < target.getNumSamples(); ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * 300.0 * static_cast<double> (startSample + i) / 48000.0;
            const auto value = 0.5f * static_cast<float> (std::sin (phase));
            left[i] = value;
            right[i] = -value;
        }
    });

    CHECK (engine.getCorrelationValue() < -0.9f);
    CHECK (engine.getOutputCorrelationValue() < -0.9f);

    // ...then 8 s of silence: a raw leaky Pearson ratio would hold -1
    // forever (numerator and denominator decay at the same rate); the
    // v0.3.0 energy gate must decay every displayed estimate toward 0.
    processBlocks (engine, buffer, 188, [] (juce::AudioBuffer<float>& target, juce::int64)
    {
        target.clear();
    });

    for (const auto value : { engine.getCorrelationValue(), engine.getCorrelationLowValue(),
                              engine.getCorrelationHighValue(), engine.getCorrelationMidBandValue(),
                              engine.getCorrelationHighBandValue(), engine.getOutputCorrelationValue() })
    {
        CAPTURE (value);
        CHECK (std::isfinite (value));
        CHECK (std::abs (value) < 0.1f);
    }
}

TEST_CASE ("Per-band and output correlation meters: identical channels read +1, polarity-flipped read -1", "[dsp][engine][correlation][v0.3.0]")
{
    const auto measure = [] (bool flipRight)
    {
        auto enginePtr = std::make_unique<FirmamentEngine>();
        auto& engine = *enginePtr;
        engine.setBassMonoFrequencyHz (120.0f); // engage both input splits' band definitions
        engine.setHighSplitFrequencyHz (2500.0f);
        engine.setLowWidthPercent (100.0f);
        engine.setOutputDb (0.0f);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = 48000.0;
        spec.maximumBlockSize = 2048;
        spec.numChannels = 2;
        engine.prepare (spec);

        juce::AudioBuffer<float> buffer (2, 2048);
        TestHelpers::DeterministicPinkNoise pink (818181u);

        for (int block = 0; block < 48; ++block)
        {
            auto* left = buffer.getWritePointer (0);
            auto* right = buffer.getWritePointer (1);

            for (int i = 0; i < 2048; ++i)
            {
                const auto value = 0.4f * pink.nextSample();
                left[i] = value;
                right[i] = flipRight ? -value : value;
            }

            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);
        }

        return enginePtr;
    };

    {
        const auto engine = measure (false);
        CHECK (engine->getCorrelationLowValue() == Catch::Approx (1.0f).margin (0.01));
        CHECK (engine->getCorrelationHighValue() == Catch::Approx (1.0f).margin (0.01));
        CHECK (engine->getCorrelationMidBandValue() == Catch::Approx (1.0f).margin (0.01));
        CHECK (engine->getCorrelationHighBandValue() == Catch::Approx (1.0f).margin (0.01));
        CHECK (engine->getOutputCorrelationValue() == Catch::Approx (1.0f).margin (0.01));
    }

    {
        const auto engine = measure (true);
        CHECK (engine->getCorrelationLowValue() == Catch::Approx (-1.0f).margin (0.01));
        CHECK (engine->getCorrelationHighValue() == Catch::Approx (-1.0f).margin (0.01));
        CHECK (engine->getCorrelationMidBandValue() == Catch::Approx (-1.0f).margin (0.01));
        CHECK (engine->getCorrelationHighBandValue() == Catch::Approx (-1.0f).margin (0.01));
    }
}

TEST_CASE ("Processor exposes the v0.3.0 per-band and output correlation meters as atomics refreshed per block", "[processor][correlation][v0.3.0]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    TestHelpers::DeterministicPinkNoise pink (272727u);

    for (int block = 0; block < 200; ++block)
    {
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < 512; ++i)
        {
            const auto value = 0.4f * pink.nextSample();
            left[i] = value;
            right[i] = value;
        }

        processor.processBlock (buffer, midi);
    }

    CHECK (processor.getCorrelationMeterValue() == Catch::Approx (1.0f).margin (0.01));
    CHECK (processor.getCorrelationMeterLowValue() == Catch::Approx (1.0f).margin (0.05));
    CHECK (processor.getCorrelationMeterHighValue() == Catch::Approx (1.0f).margin (0.05));
    CHECK (processor.getCorrelationMeterMidBandValue() == Catch::Approx (1.0f).margin (0.05));
    CHECK (processor.getCorrelationMeterHighBandValue() == Catch::Approx (1.0f).margin (0.05));
    CHECK (processor.getOutputCorrelationMeterValue() == Catch::Approx (1.0f).margin (0.05));
}
