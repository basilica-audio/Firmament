#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Suite-reusable two-state lever toggle for the wave-3 COMPOSITIONAL
// faceplate generation (empty family plate + composited control sprites,
// campaign 2026-08: .scaffold/gui-assets/rollout-2026-07 + sprite-library).
//
// Unlike the baked-master generation's toggle handling (silentium's
// master-05/master-06 variant-pair zone swap, aureate's per-toggle
// toggle_N_down.png diff sprites), this component owns its toggle's ENTIRE
// visual: the extracted family toggle sprite (sprite-library/toggle-up.png,
// lever up = ON, dark recessed housing on its own feathered basalt patch)
// is drawn by paintButton() at the manifest position, so nothing about the
// toggle is baked into the plate underneath.
//
// KNOWN ASSET GAP (sprite-library/provenance.md "Known gaps"): no approved
// lever-DOWN source render exists in the whole campaign - the toggle-down
// master variant (blueprint section 2c) was never produced because no base
// master was accepted in wave 1. Until that dedicated component render
// lands, the OFF state is this same sprite drawn VERTICALLY MIRRORED about
// the housing's own centre line (a pure runtime transform of the approved
// asset - the asset itself is never relit or repainted, per the campaign's
// no-relighting rule). The mirror necessarily flips the sprite's baked
// key-light direction within the housing recess; that is the documented,
// accepted cost of the nearest-sprite workaround, flagged for replacement
// once a real down-state sprite exists.
namespace basilica::gui
{
    class SpriteToggle : public juce::Button
    {
    public:
        // spriteImage: the full toggle sprite (feathered basalt patch
        // included). anchorInSpritePx: the housing centre in the sprite's
        // own pixel space - the point the layout manifest's cx/cy refers
        // to, and the line the OFF-state mirror flips about.
        SpriteToggle (juce::Image spriteImage, juce::Point<float> anchorInSpritePx,
                      juce::String accessibleTitle)
            : juce::Button (accessibleTitle),
              sprite (std::move (spriteImage)), anchor (anchorInSpritePx)
        {
            setClickingTogglesState (true);
            setTitle (accessibleTitle);
            setWantsKeyboardFocus (true);
        }

        // Where the housing centre lands within this component's local
        // bounds, as fractions - resized()/the editor sizes this component
        // to the sprite's full footprint, so the anchor fraction is simply
        // the sprite-space anchor normalised by the sprite's canvas.
        juce::Point<float> anchorFraction() const noexcept
        {
            if (sprite.isValid())
                return { anchor.x / (float) sprite.getWidth(), anchor.y / (float) sprite.getHeight() };

            return { 0.5f, 0.5f };
        }

        void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override
        {
            juce::ignoreUnused (shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

            if (! sprite.isValid())
                return;

            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

            const auto bounds = getLocalBounds().toFloat();
            const auto scale = bounds.getWidth() / (float) sprite.getWidth();

            auto transform = juce::AffineTransform::scale (scale);

            if (! getToggleState())
            {
                // OFF: mirror about the housing centre's own horizontal
                // axis (see the top-of-file asset-gap docs). Flipping about
                // the ANCHOR line (not the bounds centre) keeps the housing
                // itself registered on the manifest position even though
                // the anchor is not the sprite's geometric centre.
                const auto anchorYLocal = anchor.y * scale;
                transform = transform.followedBy (
                    juce::AffineTransform::verticalFlip (0.0f).translated (0.0f, 2.0f * anchorYLocal));
            }

            g.drawImageTransformed (sprite, transform);

            // WCAG 2.4.7 Focus Visible: paintButton() fully replaces the
            // LookAndFeel button draw, so the focus indicator must be drawn
            // here (same self-contained convention as MasterCropKnob).
            if (hasKeyboardFocus (true))
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawRoundedRectangle (bounds.reduced (2.0f), 4.0f, 1.5f);
            }
        }

    private:
        juce::Image sprite;
        juce::Point<float> anchor;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpriteToggle)
    };
}
