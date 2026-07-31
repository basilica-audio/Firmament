// v0.3.0 real-time-safety guarantee (binding brief, section 6.11):
// processBlock()/FirmamentEngine::process() must not touch the heap once
// prepared, under EVERY mode combination - including the Linear Phase
// convolution path and mode switches mid-run. Message-thread kernel loads
// via Convolution::loadImpulseResponse are expected (and exempt): the guard
// brackets only the render calls. Also exercises the oversized-block chunk
// guard with 16384-sample blocks.

#include "AllocationGuard.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

// Cross-thread hardening follow-up (tests/CrossThreadReprepareTests.cpp):
// every existing test case in this file only ever asserts
// AllocationGuard::allocationCount() == 0, which would pass vacuously if the
// guard's operator new/delete overrides were silently not firing at all (a
// broken/optimised-away hook, an ODR issue, etc. - the same vacuous-guard
// trap previously found in sibling basilica-audio/requiem, see
// tests/EngineTests.cpp's "6.12 The allocation guard itself works"). Ported
// here below, unchanged in method: the storage MUST come from a direct call
// to the replaced ::operator new (a plain function call, not a
// new-expression, so [expr.new]'s elision permission for new-expressions
// whose storage is never observably used does not apply), and MUST be
// written through a volatile pointer, so the allocation is observably used
// and cannot be optimised away even in a Release build.
TEST_CASE ("AllocationGuard itself detects a real allocation and does not fire on pure computation", "[robustness][realtime][allocation][v0.3.0]")
{
    {
        AllocationGuard::reset();

        auto* deliberate = static_cast<float*> (::operator new (64 * sizeof (float)));
        *static_cast<volatile float*> (deliberate) = 1.0f;
        ::operator delete (deliberate);

        CHECK (AllocationGuard::allocationCount() > 0);
    }

    {
        AllocationGuard::reset();

        volatile float sum = 0.0f;
        for (int i = 0; i < 1000; ++i)
            sum = sum + static_cast<float> (i);

        CHECK (AllocationGuard::allocationCount() == 0);
    }
}

namespace
{
    void setParam (FirmamentAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }

    void fillProgram (juce::AudioBuffer<float>& buffer,
                      TestHelpers::DeterministicPinkNoise& pinkLeft,
                      TestHelpers::DeterministicPinkNoise& pinkRight)
    {
        TestHelpers::fillStereoWithDeterministicPinkNoise (buffer, pinkLeft, pinkRight, 0.35f);
    }
}

TEST_CASE ("processBlock() performs zero heap allocations under every mode combination", "[robustness][realtime][allocation][v0.3.0]")
{
    // Every stage engaged at once wherever legal; the three-way selectors
    // are iterated below.
    FirmamentAudioProcessor processor;
    setParam (processor, ParamIDs::width, 160.0f);
    setParam (processor, ParamIDs::lowWidth, 60.0f);
    setParam (processor, ParamIDs::bassMonoFreq, 120.0f);
    setParam (processor, ParamIDs::highSplitFreq, 2500.0f);
    setParam (processor, ParamIDs::highWidth, 140.0f);
    setParam (processor, ParamIDs::autoMonoSafety, 1.0f);
    setParam (processor, ParamIDs::autoMonoSafetyMultiband, 1.0f);
    setParam (processor, ParamIDs::decorrelateEnabled, 1.0f);
    setParam (processor, ParamIDs::haasEnabled, 1.0f);
    setParam (processor, ParamIDs::widthComp, 1.0f);
    setParam (processor, ParamIDs::monoAudition, 1.0f);

    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    TestHelpers::DeterministicPinkNoise pinkLeft (1010u), pinkRight (2020u);

    for (int decorrelateMode = 0; decorrelateMode < 3; ++decorrelateMode)
    {
        for (int bassMonoMode = 0; bassMonoMode < 3; ++bassMonoMode)
        {
            for (int safetyMode = 0; safetyMode < 2; ++safetyMode)
            {
                // Parameter changes + Linear Phase kernel servicing happen
                // OUTSIDE the guarded bracket (message-thread work, exempt
                // per the brief).
                setParam (processor, ParamIDs::decorrelateMode, static_cast<float> (decorrelateMode));
                setParam (processor, ParamIDs::bassMonoMode, static_cast<float> (bassMonoMode));
                setParam (processor, ParamIDs::safetyMode, static_cast<float> (safetyMode));
                processor.handleMessageThreadServicing (true);

                // Warmup: crossfades/mode transitions (including the Linear
                // Phase path swap + its reset()) run inside processBlock and
                // must be allocation-free too - so only the *first* block
                // after the parameter change is unguarded (it may pick up
                // the new values), everything after is measured.
                fillProgram (buffer, pinkLeft, pinkRight);
                processor.processBlock (buffer, midi);

                AllocationGuard::reset();

                for (int block = 0; block < 16; ++block)
                {
                    fillProgram (buffer, pinkLeft, pinkRight);
                    processor.processBlock (buffer, midi);
                }

                // Snapshot the count BEFORE Catch2's CAPTURE machinery runs:
                // CAPTURE itself allocates (string conversions) on this same
                // thread, which would otherwise leak into the measurement.
                const auto allocationsDuringRender = AllocationGuard::allocationCount();

                CAPTURE (decorrelateMode, bassMonoMode, safetyMode);
                CHECK (allocationsDuringRender == 0);
                REQUIRE (TestHelpers::allSamplesFinite (buffer));
            }
        }
    }
}

TEST_CASE ("FirmamentEngine::process() performs zero heap allocations, including mode switches mid-run inside the guarded region", "[robustness][realtime][allocation][v0.3.0]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (150.0f);
    engine.setLowWidthPercent (70.0f);
    engine.setBassMonoFrequencyHz (120.0f);
    engine.setHighSplitFrequencyHz (3000.0f);
    engine.setHighWidthPercent (130.0f);
    engine.setAutoMonoSafetyEnabled (true);
    engine.setDecorrelateEnabled (true);
    engine.setOutputDb (0.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 2;
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, 512);
    TestHelpers::DeterministicPinkNoise pinkLeft (3030u), pinkRight (4040u);

    for (int warmup = 0; warmup < 8; ++warmup)
    {
        fillProgram (buffer, pinkLeft, pinkRight);
        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);
    }

    // Engine-level setters are audio-thread-legal (called from processBlock
    // in production), so mode switches happen INSIDE the guarded region
    // here - including switches into/out of Linear Phase, whose path swap
    // and LinearPhaseCrossover::reset() must be allocation-free.
    AllocationGuard::reset();

    for (int block = 0; block < 48; ++block)
    {
        if (block % 6 == 0)
        {
            engine.setDecorrelateMode (block / 6 % 3);
            engine.setBassMonoMode (block / 6 % 3);
            engine.setSafetyMode (block / 6 % 2);
        }

        fillProgram (buffer, pinkLeft, pinkRight);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    CHECK (AllocationGuard::allocationCount() == 0);
    REQUIRE (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Oversized-block chunk guard: 16384-sample blocks through a 512-prepared engine stay allocation-free and finite", "[robustness][realtime][allocation][v0.3.0]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (150.0f);
    engine.setBassMonoFrequencyHz (120.0f);
    engine.setHighSplitFrequencyHz (2500.0f);
    engine.setDecorrelateEnabled (true);
    engine.setDecorrelateMode (static_cast<int> (FirmamentEngine::DecorrelateMode::velvetDense));
    engine.setOutputDb (0.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512; // deliberately smaller than the blocks below
    spec.numChannels = 2;
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, 16384);
    TestHelpers::DeterministicPinkNoise pinkLeft (5050u), pinkRight (6060u);

    for (int warmup = 0; warmup < 2; ++warmup)
    {
        fillProgram (buffer, pinkLeft, pinkRight);
        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);
    }

    AllocationGuard::reset();

    for (int block = 0; block < 4; ++block)
    {
        fillProgram (buffer, pinkLeft, pinkRight);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    CHECK (AllocationGuard::allocationCount() == 0);
    CHECK (TestHelpers::allSamplesFinite (buffer));
    CHECK (TestHelpers::peakAbsolute (buffer) < 10.0f);
}