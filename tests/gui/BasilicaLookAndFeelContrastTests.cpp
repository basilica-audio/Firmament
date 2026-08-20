#include "gui/BasilicaLookAndFeel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

// A-03 pattern (WCAG 1.4.3 Contrast (Minimum), AA): pure-function WCAG
// relative-luminance contrast checks on the exact colour pairs the M3
// vector editor actually renders, fetched via BasilicaLookAndFeel's static
// accessors rather than hand-copied second literals that could silently
// drift out of sync with the ones drawn. Because this editor is 100%
// vector (no baked artwork), EVERY rendered text/marking pair is covered
// here - there is no "measured asset bound" escape hatch like the
// photoreal siblings need.
namespace
{
    // WCAG 2.x relative luminance (https://www.w3.org/TR/WCAG21/#dfn-relative-luminance)
    // applied to a juce::Colour's sRGB channels.
    double relativeLuminanceChannel (juce::uint8 sRGB8)
    {
        const auto c = (double) sRGB8 / 255.0;
        return c <= 0.03928 ? c / 12.92 : std::pow ((c + 0.055) / 1.055, 2.4);
    }

    double relativeLuminance (juce::Colour colour)
    {
        return 0.2126 * relativeLuminanceChannel (colour.getRed())
             + 0.7152 * relativeLuminanceChannel (colour.getGreen())
             + 0.0722 * relativeLuminanceChannel (colour.getBlue());
    }

    // WCAG contrast ratio (https://www.w3.org/TR/WCAG21/#dfn-contrast-ratio):
    // (L1 + 0.05) / (L2 + 0.05), L1 the lighter - order-independent by
    // construction (max/min).
    double contrastRatio (juce::Colour a, juce::Colour b)
    {
        const auto lA = relativeLuminance (a);
        const auto lB = relativeLuminance (b);
        const auto lighter = std::max (lA, lB);
        const auto darker = std::min (lA, lB);
        return (lighter + 0.05) / (darker + 0.05);
    }

    void checkTextPair (juce::Colour text, juce::Colour background, const char* what)
    {
        INFO (what << ": text " << text.toDisplayString (true).toStdString()
                   << " on " << background.toDisplayString (true).toStdString()
                   << " = " << contrastRatio (text, background) << ":1");

        // WCAG 1.4.3 (AA): the suite's 14 px serif is normal-size text
        // (below the ~18.66 px / 14 pt-bold "large text" threshold), so the
        // stricter 4.5:1 floor applies.
        CHECK (contrastRatio (text, background) >= 4.5);

        // The background must be fully opaque - a translucent surface would
        // make the REAL rendered contrast depend on whatever sits
        // underneath, defeating the guarantee.
        CHECK (background.isOpaque());
    }
}

TEST_CASE ("WCAG contrast ratio helper matches known reference values", "[gui][a11y]")
{
    // Black vs white is the canonical maximum-contrast pair: exactly 21:1.
    CHECK (contrastRatio (juce::Colours::black, juce::Colours::white) == Catch::Approx (21.0).margin (0.01));

    // Identical colours are always exactly 1:1.
    CHECK (contrastRatio (juce::Colours::grey, juce::Colours::grey) == Catch::Approx (1.0).margin (0.001));

    // Symmetric in its arguments.
    const auto a = juce::Colour (0xfff0d38c);
    const auto b = juce::Colour (0xff17110c);
    CHECK (contrastRatio (a, b) == Catch::Approx (contrastRatio (b, a)).margin (1.0e-9));
}

TEST_CASE ("Every rendered text pair of the vector editor clears WCAG AA 4.5:1", "[gui][a11y]")
{
    using LNF = basilica::gui::BasilicaLookAndFeel;

    // Control labels + toggle legends over the bus-panel fill.
    checkTextPair (LNF::getLabelTextColour(), LNF::getPanelBackgroundColour(),
                   "label/legend on panel");

    // Engraved bus headers over the panel fill.
    checkTextPair (LNF::getPanelHeaderTextColour(), LNF::getPanelBackgroundColour(),
                   "bus header on panel");

    // Slider value boxes: gold value text on the recessed chip.
    checkTextPair (LNF::getValueTextColour(), LNF::getValueBoxBackgroundColour(),
                   "value text on value box");

    // Preset-bar button lettering on the button face.
    checkTextPair (LNF::getButtonTextColour(), LNF::getButtonFaceColour(),
                   "button text on button face");

    // Needle-meter scale markings/numerals + legend on the meter face.
    checkTextPair (LNF::getMeterMarkingColour(), LNF::getMeterFaceColour(),
                   "meter markings on meter face");
}

TEST_CASE ("Non-text state indicators clear WCAG 1.4.11's 3:1 against their background", "[gui][a11y]")
{
    using LNF = basilica::gui::BasilicaLookAndFeel;

    // The knob pointer IS the control-state display ("readability of
    // control state", CLAUDE.md) - hold it to the full 4.5:1 text bar, not
    // just the 3:1 non-text floor.
    INFO ("knob pointer on knob face = "
          << contrastRatio (LNF::getKnobPointerColour(), LNF::getKnobFaceColour()) << ":1");
    CHECK (contrastRatio (LNF::getKnobPointerColour(), LNF::getKnobFaceColour()) >= 4.5);
    CHECK (LNF::getKnobFaceColour().isOpaque());

    // The meter needle against its face.
    CHECK (contrastRatio (LNF::getMeterNeedleColour(), LNF::getMeterFaceColour()) >= 4.5);

    // The focus ring (WCAG 2.4.7 indicator) against the panel fill and the
    // editor background - 3:1 per 1.4.11 Non-text Contrast. The dark halo
    // painted beneath it (see paintFocusRing) keeps it legible over the
    // gold-lit lamp toggles too.
    CHECK (contrastRatio (LNF::getFocusRingColour(), LNF::getPanelBackgroundColour()) >= 3.0);
    CHECK (contrastRatio (LNF::getFocusRingColour(), LNF::getEditorBackgroundColour()) >= 3.0);

    // Both fills the editor actually paints under controls are opaque.
    CHECK (LNF::getPanelBackgroundColour().isOpaque());
    CHECK (LNF::getEditorBackgroundColour().isOpaque());
    CHECK (LNF::getMeterFaceColour().isOpaque());
}
