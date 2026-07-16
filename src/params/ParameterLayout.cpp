#include "ParameterLayout.h"
#include "ParameterIds.h"

namespace frmm
{
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        //======================================================================
        // Width: Mid/Side width scale, 0-200%, default 100% (unity, i.e. an
        // unmodified pass-through of the input's stereo image).
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::width, 1 },
            "Width",
            juce::NormalisableRange<float> (0.0f, 200.0f, 0.1f),
            100.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        //======================================================================
        // Bass Mono Freq: crossover frequency below which the Side channel is
        // forced to mono, 0-600 Hz, default 0 Hz (off). A skew factor below 1
        // gives finer control resolution over the low, most commonly-used
        // part of the range (typical bass-mono corners sit around 80-200 Hz)
        // while still reaching all the way up to 600 Hz. The 500 -> 600 Hz
        // ceiling is a v0.2.0 range/skew refinement (docs/design-brief.md,
        // the single lowest-confidence change in that brief - reasoned from
        // the Waves S1 Shuffle control's documented ~600 Hz convention, an
        // indirect/third-party-summarised source, not a primary manual);
        // ParameterIds.h's frozen-ID rule explicitly permits range/skew
        // tuning pre-1.0, only the ID itself is frozen.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::bassMonoFreq, 1 },
            "Bass Mono Freq",
            juce::NormalisableRange<float> (0.0f, 600.0f, 0.1f, 0.4f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));

        //======================================================================
        // Output: trim applied after M/S decode back to L/R.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::output, 1 },
            "Output",
            juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));

        //======================================================================
        // M1 additions below - see ParameterIds.h for the full rationale of
        // each. All default to values that reproduce the v0.1 signal path
        // exactly (Low Width 0%, Auto Mono Safety off, Haas Mode off), so
        // existing behaviour/tests are unaffected unless a user or preset
        // opts in.

        // Low Width: independent width scale for the band below
        // BassMonoFreq, 0-200%, default 0% (matches the v0.1 "bass mono"
        // behaviour of forcing that band to mono).
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::lowWidth, 1 },
            "Low Width",
            juce::NormalisableRange<float> (0.0f, 200.0f, 0.1f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        // Auto Mono Safety: off by default.
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::autoMonoSafety, 1 },
            "Auto Mono Safety",
            false));

        // Haas Mode: off by default.
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::haasEnabled, 1 },
            "Haas Mode",
            false));

        // Haas Time: 0-40 ms, default 20 ms (a typical precedence-effect
        // sweet spot - audibly wide without reading as a discrete echo).
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::haasTimeMs, 1 },
            "Haas Time",
            juce::NormalisableRange<float> (0.0f, 40.0f, 0.01f),
            20.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));

        //======================================================================
        // v0.2.0 additions below (docs/design-brief.md) - see ParameterIds.h
        // for the full rationale of each. All default to values that
        // reproduce v0.1.1 behaviour exactly (Floor -9.1 dB matches the old
        // hardcoded 0.35 linear floor, Multiband off, Decorrelate off), so
        // existing behaviour/tests/presets are unaffected unless a user or
        // preset opts in.

        // Auto Mono Safety Floor: -24 to 0 dB, default -9.1 dB (reproduces
        // v0.1.1's hardcoded 0.35 linear floor gain bit-for-bit).
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::autoMonoSafetyFloorDb, 1 },
            "Auto Mono Safety Floor",
            juce::NormalisableRange<float> (-24.0f, 0.0f, 0.01f),
            -9.1f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));

        // Auto Mono Safety Multiband: off by default.
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::autoMonoSafetyMultiband, 1 },
            "Auto Mono Safety Multiband",
            false));

        // Decorrelate: off by default.
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::decorrelateEnabled, 1 },
            "Decorrelate",
            false));

        // Decorrelate Amount: 0-100%, default 50%.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::decorrelateAmount, 1 },
            "Decorrelate Amount",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            50.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        return layout;
    }
}
