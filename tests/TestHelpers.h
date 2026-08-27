#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <functional>
#include <vector>

// Small shared helpers used across the Tests target.
namespace TestHelpers
{
    // ======================================================================
    // Deterministic, cross-platform bit-exact pink-ish noise (Voss-McCartney
    // structure), used by the v0.3.0 state-migration null tests: every value
    // is a multiple of 2^-15 in [-1, 1), sums of up to 17 such values are
    // exactly representable in float, and the final scale is a power of two,
    // so every produced sample is bit-identical on any IEEE-754 platform
    // regardless of compiler/libm.
    //
    // MUST stay textually identical to the generator that produced the
    // checked-in v0.2.0 reference render
    // (tests/fixtures/v020-reference-render.f32) - see
    // tests/fixtures/README.md.
    struct DeterministicPinkNoise
    {
        explicit DeterministicPinkNoise (juce::uint32 seed) : state (seed)
        {
            for (auto& row : rows)
                row = nextQuantised();
        }

        float nextQuantised() noexcept
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<float> (static_cast<int> (state >> 16) - 32768) / 32768.0f;
        }

        float nextSample() noexcept
        {
            ++counter;

            int rowIndex = 0;
            for (juce::uint32 c = counter; (c & 1u) == 0u && rowIndex < numRows - 1; c >>= 1)
                ++rowIndex;

            rows[static_cast<size_t> (rowIndex)] = nextQuantised();

            float sum = nextQuantised();
            for (const auto row : rows)
                sum += row;

            return sum * (1.0f / 16.0f);
        }

        static constexpr int numRows = 16;
        juce::uint32 state = 1u;
        juce::uint32 counter = 0;
        float rows[static_cast<size_t> (numRows)] {};
    };

    // Fills a stereo buffer from two independent pink-noise generators
    // (persistent across calls for phase-continuous streams).
    inline void fillStereoWithDeterministicPinkNoise (juce::AudioBuffer<float>& buffer,
                                                      DeterministicPinkNoise& generatorLeft,
                                                      DeterministicPinkNoise& generatorRight,
                                                      float amplitude = 0.35f)
    {
        jassert (buffer.getNumChannels() >= 2);

        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            left[i] = amplitude * generatorLeft.nextSample();
            right[i] = amplitude * generatorRight.nextSample();
        }
    }

    // ======================================================================
    // The frozen v0.2.0 render protocol shared by the migration null tests
    // (StateTests.cpp) and the sentinel null test (MultibandWidthTests.cpp)
    // - see tests/fixtures/README.md. Templated so this header does not
    // depend on PluginProcessor.h.
    namespace MigrationProtocol
    {
        constexpr double sampleRate = 48000.0;
        constexpr int blockSize = 512;
        constexpr int numBlocks = 469; // 240128 samples, ~5.003 s
        constexpr float amplitude = 0.35f;
        constexpr juce::uint32 seedLeft = 123456789u;
        constexpr juce::uint32 seedRight = 987654321u;
        constexpr size_t totalSamples = static_cast<size_t> (numBlocks) * blockSize;

        // Renders the deterministic stimulus through a processor (calling
        // prepareToPlay itself) and returns [all left][all right] samples.
        template <typename Processor>
        std::vector<float> render (Processor& processor)
        {
            processor.prepareToPlay (sampleRate, blockSize);

            DeterministicPinkNoise pinkLeft (seedLeft);
            DeterministicPinkNoise pinkRight (seedRight);

            std::vector<float> outLeft, outRight;
            outLeft.reserve (totalSamples);
            outRight.reserve (totalSamples);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            for (int block = 0; block < numBlocks; ++block)
            {
                fillStereoWithDeterministicPinkNoise (buffer, pinkLeft, pinkRight, amplitude);
                processor.processBlock (buffer, midi);

                const auto* outL = buffer.getReadPointer (0);
                const auto* outR = buffer.getReadPointer (1);
                outLeft.insert (outLeft.end(), outL, outL + blockSize);
                outRight.insert (outRight.end(), outR, outR + blockSize);
            }

            outLeft.insert (outLeft.end(), outRight.begin(), outRight.end());
            return outLeft;
        }

        // The ARCHITECTURE-dependent cross-version tolerance against the
        // frozen reference file (brief 6.1b): the reference was generated on
        // macOS on Apple Silicon, so exactly that configuration asserts the
        // full -140 dBFS bound; every other executing architecture gets the
        // looser floor (libm/codegen ULP drift through IIR feedback).
        //
        // GATED ON THE EXECUTING ARCHITECTURE, NOT THE OS (issue #36). The
        // previous JUCE_WINDOWS gate was an OS check wearing an architecture
        // assumption - the same misattribution issue #100 fixed in Crypta's
        // GoldenRenderTests. What actually drifts is x86 arithmetic: the
        // x86_64 slice of this very Universal Binary, run under Rosetta 2 on
        // the same Mac that passes natively (`arch -x86_64 ctest`), lands at
        // a measured peak residual of 1.207e-6 (-118 dBFS) - all but
        // identical to MSVC's 1.24e-6 on windows-latest - while wearing
        // JUCE_MAC. CI's Rosetta runs are pluginval/auval only today, but a
        // local Rosetta ctest is a legitimate run and must not fail on
        // unmodified main. Because each slice of a Universal Binary compiles
        // separately, the compile-time JUCE_MAC && JUCE_ARM check IS the
        // executing architecture at run time - macOS picks the slice, the
        // slice picks the bound.
        //
        // BOTH VALUES ARE UNCHANGED from the OS-gated version; only WHICH
        // configuration asserts which bound moved. The -108 dBFS floor keeps
        // ~3x headroom over the measured x86 residuals so the bound is not
        // marginally flaky, and is still ~50 dB below the -60 dBFS floor of
        // anything audible. This relaxes ONLY the cross-compiler comparison
        // against the frozen Apple Silicon reference; the same-binary
        // bit-exactness assertions in MultibandWidthTests/StateTests are
        // untouched and remain exact on every architecture.
        inline constexpr float crossVersionTolerance()
        {
#if JUCE_MAC && JUCE_ARM
            return 1.0e-7f; // -140 dBFS - the reference platform itself
#else
            return 4.0e-6f; // -108 dBFS - measured x86 drift + ~3x headroom
#endif
        }
    }

    // ======================================================================
    // Message-loop pumping for the Linear Phase determinism protocol (brief
    // 6.3/6.4): TestMain.cpp holds only a ScopedJuceInitialiser_GUI, so no
    // dispatch loop runs unless a test pumps it. This runs the loop briefly
    // so pending async updates/timers fire and background threads get
    // scheduled.
    inline void pumpMessageLoop (int milliseconds = 10)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
    }

    // Polls `epoch` until it reaches `targetEpoch`, alternately pumping the
    // message loop, invoking `service` (the message-thread kernel-handoff
    // hop) and `processAudio` (the convolution installs new IRs inside its
    // process path). Returns false on timeout - callers must REQUIRE(true)
    // the result so a stalled handoff fails loudly instead of hanging.
    inline bool waitForKernelEpoch (const std::function<juce::uint64()>& epoch,
                                    juce::uint64 targetEpoch,
                                    const std::function<void()>& service,
                                    const std::function<void()>& processAudio,
                                    int timeoutMilliseconds = 10000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (timeoutMilliseconds);

        while (epoch() < targetEpoch)
        {
            if (juce::Time::getMillisecondCounter() > deadline)
                return false;

            service();
            pumpMessageLoop (2);
            processAudio();
        }

        return true;
    }

    // ======================================================================
    // Spectrum analysis helpers (v0.3.0 test plan): magnitude spectrum of a
    // real signal (Hann-windowed) and third-octave band averaging.

    // Returns |X(k)| for k = 0..fftSize/2 of the first fftSize samples.
    inline std::vector<float> magnitudeSpectrum (const std::vector<float>& samples, int fftOrder, bool applyHannWindow = true)
    {
        const auto fftSize = 1 << fftOrder;
        jassert (static_cast<int> (samples.size()) >= fftSize);

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> data (static_cast<size_t> (fftSize) * 2, 0.0f);

        for (int i = 0; i < fftSize; ++i)
        {
            const auto window = applyHannWindow
                                    ? 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * static_cast<float> (i) / static_cast<float> (fftSize - 1))
                                    : 1.0f;
            data[static_cast<size_t> (i)] = samples[static_cast<size_t> (i)] * window;
        }

        fft.performFrequencyOnlyForwardTransform (data.data(), true);
        data.resize (static_cast<size_t> (fftSize) / 2 + 1);
        return data;
    }

    // Averages bin *power* into the standard 30 third-octave bands
    // (centres 1000 * 2^((band - 16) / 3) Hz ~ 24.8 Hz .. 20.2 kHz) and
    // returns each band's average magnitude in dB. Bands without any bin
    // (possible at low rates/small FFTs for the lowest bands) return
    // -300 dB and should be skipped by callers via the `valid` flags.
    struct ThirdOctaveSpectrum
    {
        std::array<double, 30> bandDb {};
        std::array<bool, 30> valid {};
    };

    inline ThirdOctaveSpectrum thirdOctaveBandAverages (const std::vector<float>& magnitudes,
                                                        double sampleRate,
                                                        int fftSize)
    {
        ThirdOctaveSpectrum result;

        for (int band = 0; band < 30; ++band)
        {
            const auto centre = 1000.0 * std::pow (2.0, (band - 16) / 3.0);
            const auto lowEdge = centre * std::pow (2.0, -1.0 / 6.0);
            const auto highEdge = centre * std::pow (2.0, 1.0 / 6.0);

            const auto binLow = static_cast<int> (std::ceil (lowEdge * fftSize / sampleRate));
            const auto binHigh = static_cast<int> (std::floor (highEdge * fftSize / sampleRate));

            double power = 0.0;
            int count = 0;

            for (int bin = juce::jmax (1, binLow); bin <= juce::jmin (binHigh, static_cast<int> (magnitudes.size()) - 1); ++bin)
            {
                const auto magnitude = static_cast<double> (magnitudes[static_cast<size_t> (bin)]);
                power += magnitude * magnitude;
                ++count;
            }

            result.valid[static_cast<size_t> (band)] = count > 0;
            result.bandDb[static_cast<size_t> (band)] = count > 0
                                                            ? 10.0 * std::log10 (power / count + 1.0e-30)
                                                            : -300.0;
        }

        return result;
    }

    // Fills every channel of the buffer with a sine wave of the given
    // frequency. `startSampleIndex` offsets the phase calculation, so
    // calling this for consecutive blocks with startSampleIndex incremented
    // by each block's length produces a phase-continuous sine across block
    // boundaries. Defaults to 0.
    inline void fillWithSine (juce::AudioBuffer<float>& buffer,
                              double sampleRate,
                              double frequencyHz,
                              float amplitude = 0.5f,
                              juce::int64 startSampleIndex = 0)
    {
        const auto numChannels = buffer.getNumChannels();
        const auto numSamples = buffer.getNumSamples();

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int sample = 0; sample < numSamples; ++sample)
            {
                const auto phase = juce::MathConstants<double>::twoPi * frequencyHz
                                    * static_cast<double> (startSampleIndex + sample) / sampleRate;
                data[sample] = amplitude * static_cast<float> (std::sin (phase));
            }
        }
    }

    // Fills a genuinely stereo (non-mono, L != R) test signal: left channel
    // gets `leftFrequencyHz`, right channel gets `rightFrequencyHz`. Used by
    // M/S round-trip and width tests, which need real inter-channel
    // difference content (a mono source would trivially have Side == 0 and
    // wouldn't exercise the width/bass-mono stages at all).
    // `startSampleIndex` keeps the sines phase-continuous across successive
    // block-sized fills (pass `block * blockSize`); the default reproduces
    // the historical single-shot behaviour (phase 0 at the buffer start).
    inline void fillStereoWithDistinctSines (juce::AudioBuffer<float>& buffer,
                                              double sampleRate,
                                              double leftFrequencyHz,
                                              double rightFrequencyHz,
                                              float amplitude = 0.5f,
                                              juce::int64 startSampleIndex = 0)
    {
        jassert (buffer.getNumChannels() >= 2);

        const auto numSamples = buffer.getNumSamples();

        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int sample = 0; sample < numSamples; ++sample)
        {
            const auto leftPhase = juce::MathConstants<double>::twoPi * leftFrequencyHz * static_cast<double> (startSampleIndex + sample) / sampleRate;
            const auto rightPhase = juce::MathConstants<double>::twoPi * rightFrequencyHz * static_cast<double> (startSampleIndex + sample) / sampleRate;

            left[sample] = amplitude * static_cast<float> (std::sin (leftPhase));
            right[sample] = amplitude * static_cast<float> (std::sin (rightPhase));
        }
    }

    // Root-mean-square level across all channels/samples in the buffer.
    inline double rms (const juce::AudioBuffer<float>& buffer)
    {
        double sumOfSquares = 0.0;
        juce::int64 numValues = 0;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto value = static_cast<double> (data[sample]);
                sumOfSquares += value * value;
                ++numValues;
            }
        }

        return numValues > 0 ? std::sqrt (sumOfSquares / static_cast<double> (numValues)) : 0.0;
    }

    // Largest absolute sample value across all channels/samples.
    inline float peakAbsolute (const juce::AudioBuffer<float>& buffer)
    {
        float peak = 0.0f;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                peak = std::max (peak, std::abs (data[sample]));
        }

        return peak;
    }

    // Returns true if every sample in the buffer is finite (no NaN/Inf).
    inline bool allSamplesFinite (const juce::AudioBuffer<float>& buffer)
    {
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                if (! std::isfinite (data[sample]))
                    return false;
        }

        return true;
    }
}
