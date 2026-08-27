#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>

// Firmament's D4 correlation instrument (DECISIONS.md D4, binding
// 2026-07-31): a wide shallow arc dial in the family VU face language -
// cream amber-backlit face, brass bezel, centre-zero -1 / 0 / +1 scale.
// This component draws ONLY the live overlays (incandescent glow +
// rotating needle) on top of the needle-free arc-dial sprite the editor
// composites underneath it (sprite_arc_meter.png; same layering contract
// as the suite's AnalogMeter over the baked round dials).
//
// The needle is the master-diff-extracted sprite
// (resources/gui/sprite_needle_master05.png + provenance JSON) rotated
// live via juce::AffineTransform - NADEL-REGEL: needles are never baked,
// never hand-drawn, never re-profiled; rotate/scale only. The sprite is
// baked at its own extraction pose (-39.563 deg), so paint() applies
// (targetDeg - bakedAngleDeg) each frame.
//
// Value semantics: setTargetCorrelation() takes the correlation estimate
// in [-1, +1] (a plain relaxed atomic store, real-time safe from any
// thread); the ~300 ms spring ballistics run on this component's own GUI
// timer, exactly like AnalogMeter's dB ballistics.
namespace basilica::gui
{
    class ArcNeedleMeter : public juce::Component, private juce::Timer
    {
    public:
        // pivot fractions: the baked hub's position within this
        // component's bounds (the editor sizes the component to EXACTLY
        // the arc sprite's footprint, so these are the hub's sprite-space
        // fractions - see fmt::layout's arcPivot*Fraction provenance).
        ArcNeedleMeter (juce::Image needleSprite, juce::String accessibleTitle,
                        float pivotXFraction, float pivotYFraction);
        ~ArcNeedleMeter() override;

        void setTargetCorrelation (float newTarget) noexcept
        {
            targetCorrelation.store (newTarget, std::memory_order_relaxed);
        }

        // Test/preview-only: seeds both the raw target and the smoothed
        // reading immediately, bypassing the ballistic ramp a headless
        // test binary's absent message loop could never pump.
        void setImmediateCorrelationForPreview (float value) noexcept;

        float smoothedCorrelationForTest() const noexcept { return smoothedCorrelation; }

        // Correlation -> needle angle in degrees clockwise from straight
        // up. The scale is linear and centre-zero; +-1 lands on the arc
        // face's end hooks, measured on sprite_arc_meter.png at
        // +-arcEndAngleDeg from the baked hub.
        static float angleDegreesForCorrelation (float correlation) noexcept;

        // One ballistic smoothing step (same closed-form first-order
        // spring AnalogMeter uses), exposed for unit tests.
        static float stepBallistics (float current, float target, float dtSeconds, float tauSeconds) noexcept;

        void paint (juce::Graphics& g) override;

        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

        static constexpr float arcEndAngleDeg = 76.0f; // measured: end hooks at +-76 deg from the hub
        static constexpr float ballisticsTauSeconds = 0.3f;

    private:
        void timerCallback() override;

        juce::Image needle;
        juce::String title;
        float pivotXFraction, pivotYFraction;

        std::atomic<float> targetCorrelation { 0.0f };
        float smoothedCorrelation = 0.0f;
        double lastTimerSeconds = 0.0;
        double startTimeSeconds = 0.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArcNeedleMeter)
    };
}
