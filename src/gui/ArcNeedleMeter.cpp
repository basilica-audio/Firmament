#include "ArcNeedleMeter.h"
#include "Flicker.h"

#include <cmath>

namespace
{
    // sprite_needle_master05.png manifest - copied verbatim from
    // resources/gui/sprite_needle_master05.provenance.json (paint() must
    // not touch the filesystem). The sprite sits at its own extraction
    // pose; live rotation is (targetDeg - bakedAngleDeg) - see
    // ArcNeedleMeter.h's top-of-file docs.
    constexpr float needleSpriteCanvasPx = 288.0f;
    constexpr float needleBakedAngleDeg = -39.563260406580696f;

    // Master-scale parity for the needle on the ARC dial: the VU needle's
    // visible reach is 129.813 master px (provenance reachPx); the arc
    // face's scale line sits ~150 master px from the hub (measured on
    // sprite_arc_meter.png), so the needle draws at 288 * (150 / 129.813)
    // = 332.8 master px, expressed as a fraction of the arc sprite's 367
    // px canvas HEIGHT (the component's min dimension): 332.8 / 367.
    constexpr float needleSizeFraction = 332.8f / 367.0f;

    // Amber pilot-lamp glow (family face language): centred mid-face,
    // ABOVE the hub (the arc face rises above its pivot, unlike the round
    // VU dials where the glow pools below), same flicker layers as
    // AnalogMeter's glow.
    constexpr float glowCentreOffsetYFraction = -0.42f;
    constexpr float glowRadiusFraction = 0.95f;
    constexpr float glowAlphaCentre = 0.16f;
    constexpr float glowAlphaMid = 0.07f;
    constexpr float flickerAmplitudeFraction = 0.07f;

    constexpr float timerHz = 30.0f;
}

namespace basilica::gui
{
    ArcNeedleMeter::ArcNeedleMeter (juce::Image needleSprite, juce::String accessibleTitle,
                                    float pivotXFractionIn, float pivotYFractionIn)
        : needle (std::move (needleSprite)), title (std::move (accessibleTitle)),
          pivotXFraction (pivotXFractionIn), pivotYFraction (pivotYFractionIn)
    {
        setTitle (title);
        setDescription (title);

        // Pure display - never steals mouse events from controls that may
        // sit near this component's (partly transparent) bounds.
        setInterceptsMouseClicks (false, false);
        setWantsKeyboardFocus (false);

        startTimeSeconds = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        lastTimerSeconds = startTimeSeconds;

        startTimerHz ((int) timerHz);
    }

    ArcNeedleMeter::~ArcNeedleMeter()
    {
        stopTimer();
    }

    void ArcNeedleMeter::setImmediateCorrelationForPreview (float value) noexcept
    {
        targetCorrelation.store (value, std::memory_order_relaxed);
        smoothedCorrelation = value;
    }

    float ArcNeedleMeter::angleDegreesForCorrelation (float correlation) noexcept
    {
        return juce::jlimit (-1.0f, 1.0f, correlation) * arcEndAngleDeg;
    }

    float ArcNeedleMeter::stepBallistics (float current, float target, float dtSeconds, float tauSeconds) noexcept
    {
        if (! std::isfinite (target))
            return current;

        if (tauSeconds <= 0.0f || dtSeconds <= 0.0f)
            return target;

        const auto alpha = 1.0f - std::exp (-dtSeconds / tauSeconds);
        return current + (target - current) * alpha;
    }

    void ArcNeedleMeter::timerCallback()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
        const auto dt = (float) (now - lastTimerSeconds);
        lastTimerSeconds = now;

        smoothedCorrelation = stepBallistics (smoothedCorrelation,
                                              targetCorrelation.load (std::memory_order_relaxed),
                                              dt, ballisticsTauSeconds);
        repaint();
    }

    void ArcNeedleMeter::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        if (bounds.isEmpty())
            return;

        const auto pivotX = bounds.getWidth() * pivotXFraction;
        const auto pivotY = bounds.getHeight() * pivotYFraction;
        const auto halfSize = 0.5f * juce::jmin (bounds.getWidth(), bounds.getHeight());

        // 1. Incandescent pilot-lamp glow, under the needle.
        {
            const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
            const auto flicker = basilica::gui::flickerMultiplier (now, startTimeSeconds, 0.5f,
                                                                   flickerAmplitudeFraction);

            const auto glowCx = pivotX;
            const auto glowCy = pivotY + glowCentreOffsetYFraction * halfSize;
            const auto glowRadius = glowRadiusFraction * halfSize;

            juce::ColourGradient glowGradient (
                juce::Colour::fromRGB (255, 200, 120).withAlpha (juce::jlimit (0.0f, 1.0f, glowAlphaCentre * flicker)),
                glowCx, glowCy,
                juce::Colour::fromRGB (255, 170, 90).withAlpha (0.0f),
                glowCx, glowCy + glowRadius,
                true);
            glowGradient.addColour (0.5, juce::Colour::fromRGB (255, 170, 90)
                                             .withAlpha (juce::jlimit (0.0f, 1.0f, glowAlphaMid * flicker)));

            g.setGradientFill (glowGradient);
            g.fillEllipse (glowCx - glowRadius, glowCy - glowRadius, 2.0f * glowRadius, 2.0f * glowRadius);
        }

        // 2. The master-extracted needle, rotated live about the baked hub.
        if (needle.isValid())
        {
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

            const auto needleDrawSize = needleSizeFraction * juce::jmin (bounds.getWidth(), bounds.getHeight());
            const auto spriteScale = needleDrawSize / needleSpriteCanvasPx;

            const auto targetDeg = angleDegreesForCorrelation (smoothedCorrelation);
            const auto rotationRadians = juce::degreesToRadians (targetDeg - needleBakedAngleDeg);

            const auto transform = juce::AffineTransform::translation (-0.5f * (float) needle.getWidth(),
                                                                       -0.5f * (float) needle.getHeight())
                                        .scaled (spriteScale)
                                        .rotated (rotationRadians)
                                        .translated (pivotX, pivotY);

            g.drawImageTransformed (needle, transform, false);
        }
    }

    // Read-only accessible value: the current smoothed correlation, so AT
    // users get the same reading the needle shows (suite convention, see
    // AnalogMeter's MeterValueInterface).
    class ArcMeterValueInterface final : public juce::AccessibilityValueInterface
    {
    public:
        explicit ArcMeterValueInterface (ArcNeedleMeter& meterIn) : meter (meterIn) {}

        bool isReadOnly() const override { return true; }
        double getCurrentValue() const override { return (double) meter.smoothedCorrelationForTest(); }
        juce::String getCurrentValueAsString() const override
        {
            return "correlation " + juce::String (meter.smoothedCorrelationForTest(), 2);
        }
        void setValue (double) override {}
        void setValueAsString (const juce::String&) override {}
        AccessibleValueRange getRange() const override
        {
            return { { -1.0, 1.0 }, 0.01 };
        }

    private:
        ArcNeedleMeter& meter;
    };

    std::unique_ptr<juce::AccessibilityHandler> ArcNeedleMeter::createAccessibilityHandler()
    {
        return std::make_unique<juce::AccessibilityHandler> (
            *this, juce::AccessibilityRole::progressBar,
            juce::AccessibilityActions {},
            juce::AccessibilityHandler::Interfaces { std::make_unique<ArcMeterValueInterface> (*this) });
    }
}
