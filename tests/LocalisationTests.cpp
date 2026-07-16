#include "params/ParameterIds.h"

#include <BinaryData.h>
#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

// M2 i18n frame coverage (.scaffold/specs/preset-system-m2.md's "I18N"
// section): "the de mapping parses; every TRANS key present in de.txt
// (script or test iterates keys); parameter names verifiably NOT in the
// mapping." src/presets/PresetBar.cpp/PresetManager.cpp are copied verbatim
// from basilica-audio/nave (the M2 pilot - see docs/preset-system-notes.md),
// so the set of TRANS()'d keys they call is identical across every sibling
// plugin; resources/i18n/de.txt is likewise copied verbatim.
namespace
{
    // Every juce::String literal passed to TRANS() in src/presets/PresetBar.cpp
    // and src/presets/PresetManager.cpp (grep-verified against both files -
    // see the comment above). Keeping this list here (rather than trying to
    // scan the source at test time) is the "script or test iterates keys"
    // option the spec explicitly allows.
    constexpr const char* presetFrameTransKeys[] = {
        "Init",
        "Factory",
        "User",
        "Set current as default",
        "Save",
        "Save As...",
        "Delete",
        "Import...",
        "Export...",
        "Enter a name for the new preset:",
        "Preset name",
        "Cancel",
        "Import a preset or preset bank...",
        "Import failed",
        "Export preset...",
        "This file is not a valid preset.",
        "This preset was saved by an incompatible version of the preset format.",
        "This preset file belongs to a different plugin.",
    };

    // Firmament's own DSP/parameter display names (src/params/ParameterLayout.cpp) -
    // core/DSP terminology must NEVER be translated anywhere in this plugin
    // (.scaffold/specs/preset-system-m2.md's I18N section), so none of these
    // may appear as a *key* in de.txt.
    constexpr const char* parameterDisplayNames[] = {
        "Width",
        "Bass Mono Freq",
        "Output",
        "Low Width",
        "Auto Mono Safety",
        "Haas Mode",
        "Haas Time",
        "Auto Mono Safety Floor",
        "Auto Mono Safety Multiband",
        "Decorrelate",
        "Decorrelate Amount",
    };

    // A sentinel result-if-not-found string that could never legitimately
    // appear as a real German translation, so translate()'s fallback return
    // value unambiguously signals "no mapping for this key".
    constexpr const char* notFoundSentinel = "__FIRMAMENT_TEST_KEY_NOT_FOUND__";
}

TEST_CASE ("Localisation: resources/i18n/de.txt parses without error", "[i18n][presets]")
{
    REQUIRE (BinaryData::de_txtSize > 0);

    const auto text = juce::String::fromUTF8 (BinaryData::de_txt, BinaryData::de_txtSize);
    REQUIRE (text.isNotEmpty());

    // LocalisedStrings' constructor doesn't throw/return a status on
    // malformed input (JUCE 8.0.14, juce_core/text/juce_LocalisedStrings.h) -
    // a parse failure would instead silently produce empty/incomplete
    // mappings, which the "every key present" test below already catches
    // (every expected key would report as not-found). This test's own job is
    // just to confirm the file loads as a non-empty, well-formed mapping
    // with the expected "language:"/"countries:" header intact.
    const juce::LocalisedStrings mappings (text, true);

    CHECK (mappings.getLanguageName() == "German");
    CHECK (mappings.getCountryCodes().contains ("de"));
}

TEST_CASE ("Localisation: every PresetBar/PresetManager TRANS() key has a German mapping in de.txt", "[i18n][presets]")
{
    const auto text = juce::String::fromUTF8 (BinaryData::de_txt, BinaryData::de_txtSize);
    const juce::LocalisedStrings mappings (text, true);

    for (const auto* key : presetFrameTransKeys)
    {
        CAPTURE (key);
        const auto translated = mappings.translate (juce::String (key), juce::String (notFoundSentinel));
        CHECK (translated != juce::String (notFoundSentinel));
        CHECK (translated.isNotEmpty());
    }
}

TEST_CASE ("Localisation: parameter/technical display names are verifiably NOT present as keys in de.txt", "[i18n][presets]")
{
    // Core/DSP terminology - parameter names, units, technical terms - must
    // never be translated anywhere in this plugin (binding spec). Firmament's
    // knob labels (src/PluginEditor.cpp) intentionally never call TRANS() on
    // these strings in the first place, but this test guards the *data* side
    // too - de.txt itself must never grow a mapping for one of them (e.g. a
    // future contributor accidentally "translating" a parameter name).
    const auto text = juce::String::fromUTF8 (BinaryData::de_txt, BinaryData::de_txtSize);
    const juce::LocalisedStrings mappings (text, true);

    for (const auto* name : parameterDisplayNames)
    {
        CAPTURE (name);
        const auto translated = mappings.translate (juce::String (name), juce::String (notFoundSentinel));
        CHECK (translated == juce::String (notFoundSentinel));
    }
}
