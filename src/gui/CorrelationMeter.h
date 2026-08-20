#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>

// Correlation/phase needle meter for the M3 vector editor (issue #4).
//
// Fully vector-drawn (face, engraved arc, tick marks, legend, needle - all
// juce::Path/Graphics at runtime): a direct adaptation of Miserere's
// NeedleMeter (basilica-audio/miserere PR #31) from the gain-reduction
// domain to Firmament's correlation domain. The suite's needle-meter family
// (aureate -> requiem -> silentium) rotates Blender-rendered needle sprites
// over baked dial faces; Firmament has no photoreal assets at all, so both
// the face and the needle are drawn here.
//
// Threading model (same as Miserere's NeedleMeter/the suite's HubNeedle):
// setTargetCorrelation() is a plain relaxed atomic store, safe from any
// thread; ballistic smoothing runs on the GUI thread via tick(), driven by
// the editor's own timer (never a Timer owned here), so headless tests can
// advance it deterministically without a running message loop.
//
// Value convention: Pearson-style correlation in [-1, +1], matching
// FirmamentAudioProcessor::get{,Output}CorrelationMeterValue(): +1 = fully
// correlated (mono-compatible), 0 = decorrelated, -1 = fully out-of-phase
// (cancels on mono fold-down). The needle rests at the +1 right-hand
// extreme for a mono input and sweeps left as the image widens/inverts -
// the classic phase-scope needle convention.
//
// Accessibility (A-07 pattern, WCAG 4.1.2): exposes a read-only
// AccessibilityTextValueInterface with the current smoothed reading as a
// signed two-decimal string, queryable on demand - deliberately NOT
// announced on every repaint (see silentium's AnalogMeter docs for why
// auto-announce is the wrong behaviour for a meter). The component is
// display-only: it never takes keyboard focus and never intercepts mouse
// events.
namespace basilica::gui
{
    class CorrelationMeter : public juce::Component
    {
    public:
        // accessibleTitle: e.g. "Input correlation meter".
        // faceLegend: the short engraved legend on the face, e.g. "IN".
        CorrelationMeter (juce::String accessibleTitle, juce::String faceLegend);
        ~CorrelationMeter() override;

        // Thread-safe (plain relaxed atomic store): the instantaneous
        // correlation reading in [-1, +1]. Non-finite values are stored
        // as-is and sanitised at the ballistics step.
        void setTargetCorrelation (float newTargetCorrelation) noexcept
        {
            targetCorrelation.store (newTargetCorrelation, std::memory_order_relaxed);
        }

        // Advances the ballistic smoothing by dtSeconds and repaints if the
        // smoothed value changed meaningfully - called from the editor's
        // timer (see PluginEditor.cpp).
        void tick (float dtSeconds) noexcept;

        // Test/preview-only: seeds both the raw target and the smoothed
        // reading immediately, bypassing the ramp (headless test binaries
        // have no message loop to pump real ticks through).
        void setImmediateCorrelationForPreview (float correlation) noexcept;

        float getSmoothedCorrelation() const noexcept { return smoothedCorrelation; }

        void paint (juce::Graphics& g) override;
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

        // One-pole ballistic integration step, pure/static so it is
        // directly unit-testable without a running timer. Non-finite
        // targets resolve to 0 (needle centred) instead of poisoning the
        // smoothed state with NaN/inf.
        static float stepBallistics (float currentSmoothed, float target,
                                     float dtSeconds, float tauSeconds) noexcept;

        // Correlation -> needle angle in degrees, clockwise from straight-up
        // (12 o'clock). Linear over the engraved -1/-0.5/0/+0.5/+1 tick
        // scale, clamped beyond both ends; monotonically INCREASING (+1
        // rests the needle at the right-hand extreme).
        static float angleDegreesForCorrelation (float correlation) noexcept;

        static constexpr float ballisticsTauSeconds = 0.18f;

    private:
        class ValueInterface;

        const juce::String title;
        const juce::String legend;

        std::atomic<float> targetCorrelation { 0.0f };
        float smoothedCorrelation = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CorrelationMeter)
    };
}
