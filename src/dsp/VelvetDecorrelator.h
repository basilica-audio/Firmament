#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <span>
#include <vector>

// Optimized velvet-noise decorrelation (v0.3.0): a short, sparse FIR whose
// tap times/gains are the published optimal pairs from
//
//   S. J. Schlecht, B. Alary, V. Valimaki, E. A. P. Habets, "Optimized
//   Velvet-Noise Decorrelator", Proc. DAFx-18, Aveiro, 2018 (Tables 1-2),
//
// building on B. Alary, A. Politis, V. Valimaki, "Velvet-Noise Decorrelator",
// Proc. DAFx-17. The coefficients below are data transcribed from the
// open-access paper's published tables - no third-party source code is
// copied or derived from (docs/research-notes.md Section 7 carries the full
// citations).
//
// Firmament uses these as a *symmetric complementary* pair: channel A
// processes Left, channel B processes Right (post-M/S-decode), so the
// widening cost is symmetric and the centre image stays put - unlike the
// v0.2.0 R-only allpass cascade, which remains available as the "Classic"
// decorrelate mode. Two optimized pairs ship: OVN30 ("Velvet Dense", 30 taps,
// ~28 ms) and OVN15 ("Velvet Sparse", 15 taps, ~27 ms, half the cost).
//
// Structure: one circular buffer per instance (next power of two >=
// ceil(0.030 * fs) + 1 samples), inner loop y = sum(g_i * buf[(w - k_i) &
// mask]) - a pure sparse FIR. Zero latency, no denormal tail, ~30 (or 15)
// MACs per sample.
//
// Sample-rate handling (binding per the v0.3.0 brief, section 3.1): the
// published tap times are samples @44.1 kHz. prepare() rescales
// k_i = round(tau_i * fs / 44100) - keeping the ms-true timing, as DAFx-18
// shows integer rounding of the continuous locations costs < 1.3 dB of
// smoothed-magnitude error - and then renormalises the gains to exactly unit
// energy (g_i /= sqrt(sum(g_i^2))), so the filter never changes broadband
// level at any rate.
class VelvetDecorrelator
{
public:
    enum class Variant
    {
        dense30A, // OVN30 pair, channel A (Left)
        dense30B, // OVN30 pair, channel B (Right)
        sparse15A, // OVN15 pair, channel A (Left)
        sparse15B // OVN15 pair, channel B (Right)
    };

    // Allocates the circular buffer and rescales/renormalises the tap table
    // for the given sample rate. Message thread only (allocates).
    void prepare (double sampleRate, Variant variant)
    {
        const auto table = getPublishedTable (variant);

        taps.clear();
        taps.reserve (table.size());

        double energy = 0.0;

        for (const auto& tap : table)
        {
            const auto rescaledOffset = static_cast<int> (std::lround (static_cast<double> (tap.offsetAt44k1) * sampleRate / 44100.0));
            taps.push_back ({ juce::jmax (0, rescaledOffset), static_cast<float> (tap.gain) });
            energy += tap.gain * tap.gain;
        }

        const auto normalisation = energy > 0.0 ? 1.0 / std::sqrt (energy) : 1.0;

        for (auto& tap : taps)
            tap.gain = static_cast<float> (static_cast<double> (tap.gain) * normalisation);

        // Buffer sized to the documented 30 ms worst case plus one sample,
        // rounded up to a power of two for mask-based indexing; this always
        // covers the largest rescaled tap offset (max published tau is 1258
        // samples @44.1 kHz ~ 28.5 ms).
        const auto minimumSize = static_cast<int> (std::ceil (0.030 * sampleRate)) + 1;
        bufferSize = juce::nextPowerOfTwo (minimumSize);
        mask = static_cast<size_t> (bufferSize - 1);
        buffer.assign (static_cast<size_t> (bufferSize), 0.0f);
        writeIndex = 0;
    }

    // Clears the circular buffer without deallocating. Audio-thread safe.
    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    // Sparse-FIR convolution of one sample. Audio-thread safe, no allocation.
    float processSample (float input) noexcept
    {
        buffer[writeIndex & mask] = input;

        float output = 0.0f;

        for (const auto& tap : taps)
            output += tap.gain * buffer[(writeIndex - static_cast<size_t> (tap.offset)) & mask];

        ++writeIndex;
        return output;
    }

    // Test surface: the rescaled, unit-energy tap table currently in use
    // (offsets in samples at the prepared rate).
    struct Tap
    {
        int offset = 0;
        float gain = 0.0f;
    };

    const std::vector<Tap>& getTaps() const noexcept { return taps; }

private:
    struct PublishedTap
    {
        int offsetAt44k1;
        double gain;
    };

    // DAFx-18 Tables 1-2 (gains are the published gamma*10 values divided by
    // 10). tau(0) = 1 in the published tables.
    static std::span<const PublishedTap> getPublishedTable (Variant variant) noexcept
    {
        static constexpr std::array<PublishedTap, 30> ovn30A { {
            { 1, 0.471 }, { 46, 0.737 }, { 91, -0.372 }, { 134, 0.146 }, { 175, 0.112 },
            { 182, -0.184 }, { 239, 0.064 }, { 271, -0.054 }, { 351, -0.064 }, { 359, 0.108 },
            { 407, -0.032 }, { 484, 0.024 }, { 531, 0.021 }, { 536, -0.049 }, { 581, 0.014 },
            { 651, 0.018 }, { 669, -0.014 }, { 731, -0.009 }, { 797, -0.008 }, { 829, -0.008 },
            { 851, 0.007 }, { 890, 0.005 }, { 961, 0.004 }, { 984, -0.004 }, { 1027, 0.002 },
            { 1074, 0.002 }, { 1130, 0.001 }, { 1175, -0.001 }, { 1232, 0.001 }, { 1246, -0.001 },
        } };

        static constexpr std::array<PublishedTap, 30> ovn30B { {
            { 1, 0.411 }, { 5, -0.391 }, { 78, 0.558 }, { 125, 0.430 }, { 172, -0.296 },
            { 219, 0.202 }, { 234, -0.061 }, { 271, -0.134 }, { 318, 0.115 }, { 381, -0.093 },
            { 403, 0.081 }, { 460, -0.037 }, { 531, -0.026 }, { 575, 0.016 }, { 583, 0.014 },
            { 663, 0.010 }, { 703, -0.019 }, { 737, 0.007 }, { 791, 0.006 }, { 809, 0.005 },
            { 881, 0.005 }, { 902, -0.006 }, { 950, -0.004 }, { 999, 0.003 }, { 1041, 0.002 },
            { 1083, -0.002 }, { 1135, 0.001 }, { 1177, -0.001 }, { 1216, -0.001 }, { 1258, -0.001 },
        } };

        static constexpr std::array<PublishedTap, 15> ovn15A { {
            { 1, 0.480 }, { 51, -0.751 }, { 101, -0.418 }, { 200, -0.158 }, { 291, -0.048 },
            { 372, 0.029 }, { 476, 0.021 }, { 581, 0.043 }, { 627, -0.008 }, { 736, 0.020 },
            { 827, 0.012 }, { 913, 0.008 }, { 998, 0.005 }, { 1089, 0.003 }, { 1180, -0.001 },
        } };

        static constexpr std::array<PublishedTap, 15> ovn15B { {
            { 1, 0.610 }, { 10, -0.294 }, { 140, 0.663 }, { 215, -0.105 }, { 279, -0.288 },
            { 365, -0.046 }, { 485, -0.028 }, { 579, -0.068 }, { 668, -0.036 }, { 756, 0.006 },
            { 836, 0.004 }, { 892, -0.009 }, { 1005, 0.002 }, { 1071, 0.001 }, { 1192, -0.002 },
        } };

        switch (variant)
        {
            case Variant::dense30A: return ovn30A;
            case Variant::dense30B: return ovn30B;
            case Variant::sparse15A: return ovn15A;
            case Variant::sparse15B: return ovn15B;
        }

        return ovn30A;
    }

    std::vector<Tap> taps;
    std::vector<float> buffer;
    int bufferSize = 0;
    size_t mask = 0;
    size_t writeIndex = 0;
};
