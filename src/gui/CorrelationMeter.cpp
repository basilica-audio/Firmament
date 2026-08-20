#include "CorrelationMeter.h"

#include "BasilicaLookAndFeel.h"

#include <array>
#include <cmath>

namespace
{
    struct Tick
    {
        float correlation;
        float deg; // clockwise from straight-up
    };

    // Engraved correlation scale: -1 (fully out-of-phase) at the left-hand
    // extreme, +1 (mono-compatible) at the right-hand rest position. Unlike
    // Miserere's gain-reduction table this is deliberately LINEAR - the
    // correlation domain is already perceptually even, and the symmetric
    // scale keeps 0 (decorrelated) engraved exactly at 12 o'clock. The same
    // table drives BOTH the painted tick marks and the needle angle, so
    // face and needle can never drift apart.
    constexpr std::array<Tick, 5> ticks {
        Tick { -1.0f, -50.0f },
        Tick { -0.5f, -25.0f },
        Tick { 0.0f, 0.0f },
        Tick { 0.5f, 25.0f },
        Tick { 1.0f, 50.0f },
    };

    // Geometry shared by paint(): the needle hub sits below the visible
    // face bottom so the arc reads as a window onto a larger dial.
    constexpr float hubYFraction = 1.25f;          // hub centre, fraction of component height
    constexpr float needleLengthFraction = 1.08f;  // needle length, fraction of component height

    juce::String tickLabelFor (float correlation)
    {
        if (correlation == 0.0f)
            return "0";

        return (correlation > 0.0f ? juce::String ("+") : juce::String ("-"))
             + (std::abs (correlation) == 1.0f ? juce::String ("1") : juce::String (".5"));
    }
}

namespace basilica::gui
{
    CorrelationMeter::CorrelationMeter (juce::String accessibleTitle, juce::String faceLegend)
        : title (std::move (accessibleTitle)), legend (std::move (faceLegend))
    {
        setTitle (title);
        setDescription (title);

        // Pure display: never focusable, never steals mouse events from
        // controls near it.
        setWantsKeyboardFocus (false);
        setInterceptsMouseClicks (false, false);
    }

    CorrelationMeter::~CorrelationMeter() = default;

    float CorrelationMeter::angleDegreesForCorrelation (float correlation) noexcept
    {
        if (! std::isfinite (correlation))
            return ticks[2].deg; // centred (0 correlation)

        const auto clamped = juce::jlimit (ticks.front().correlation, ticks.back().correlation, correlation);
        return clamped * ticks.back().deg; // linear scale, see the tick table
    }

    float CorrelationMeter::stepBallistics (float currentSmoothed, float target,
                                            float dtSeconds, float tauSeconds) noexcept
    {
        // Sanitise: a NaN/inf reading (defensive - should never happen, the
        // engine clamps its own metering) must not poison the smoothed
        // state, and a poisoned current state must be recoverable.
        if (! std::isfinite (target))
            target = 0.0f;

        if (! std::isfinite (currentSmoothed))
            return target;

        if (tauSeconds <= 0.0f || dtSeconds <= 0.0f)
            return target;

        const auto alpha = 1.0f - std::exp (-dtSeconds / tauSeconds);
        return currentSmoothed + (target - currentSmoothed) * alpha;
    }

    void CorrelationMeter::tick (float dtSeconds) noexcept
    {
        const auto target = targetCorrelation.load (std::memory_order_relaxed);
        const auto next = stepBallistics (smoothedCorrelation, target, dtSeconds, ballisticsTauSeconds);

        if (! juce::approximatelyEqual (next, smoothedCorrelation))
        {
            smoothedCorrelation = next;
            repaint();
        }
    }

    void CorrelationMeter::setImmediateCorrelationForPreview (float correlation) noexcept
    {
        targetCorrelation.store (correlation, std::memory_order_relaxed);
        smoothedCorrelation = std::isfinite (correlation) ? correlation : 0.0f;
        repaint();
    }

    void CorrelationMeter::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();

        // --- Recessed face ---------------------------------------------
        g.setColour (BasilicaLookAndFeel::getMeterFaceColour());
        g.fillRoundedRectangle (bounds, 6.0f);

        g.setColour (BasilicaLookAndFeel::getMeterMarkingColour().withAlpha (0.6f));
        g.drawRoundedRectangle (bounds.reduced (0.75f), 6.0f, 1.5f);

        // Clip everything dial-related to the face so the below-face hub
        // geometry never paints outside the meter.
        g.saveState();
        g.reduceClipRegion (bounds.reduced (2.0f).toNearestInt());

        const auto hubX = bounds.getCentreX();
        const auto hubY = bounds.getHeight() * hubYFraction;
        const auto arcOuter = bounds.getHeight() * (needleLengthFraction + 0.06f);
        const auto arcInner = arcOuter - juce::jmin (8.0f, bounds.getHeight() * 0.09f);

        // --- Engraved arc + ticks + numerals ---------------------------
        g.setColour (BasilicaLookAndFeel::getMeterMarkingColour());

        juce::Path arc;
        arc.addCentredArc (hubX, hubY, arcInner, arcInner,
                           0.0f,
                           juce::degreesToRadians (ticks.front().deg),
                           juce::degreesToRadians (ticks.back().deg),
                           true);
        g.strokePath (arc, juce::PathStrokeType (1.2f));

        const auto numeralFont = BasilicaLookAndFeel::getSerifFont (juce::jmax (10.0f, bounds.getHeight() * 0.12f));
        g.setFont (numeralFont);

        for (const auto& tick : ticks)
        {
            const auto angle = juce::degreesToRadians (tick.deg);
            const auto sinA = std::sin (angle);
            const auto cosA = std::cos (angle);

            juce::Path tickPath;
            tickPath.startNewSubPath (hubX + arcInner * sinA, hubY - arcInner * cosA);
            tickPath.lineTo (hubX + arcOuter * sinA, hubY - arcOuter * cosA);
            g.strokePath (tickPath, juce::PathStrokeType (1.4f));

            // Numeral just inside the arc.
            const auto numeralRadius = arcInner - numeralFont.getHeight() * 0.75f;
            const auto numeralCentreX = hubX + numeralRadius * sinA;
            const auto numeralCentreY = hubY - numeralRadius * cosA;
            g.drawSingleLineText (tickLabelFor (tick.correlation),
                                  (int) std::lround (numeralCentreX),
                                  (int) std::lround (numeralCentreY),
                                  juce::Justification::horizontallyCentred);
        }

        // Legend (source + unit) engraved below the arc's centre.
        g.setFont (BasilicaLookAndFeel::getSerifFont (juce::jmax (10.0f, bounds.getHeight() * 0.13f), true));
        g.drawText (legend + "  CORR",
                    bounds.withTrimmedBottom (bounds.getHeight() * 0.06f),
                    juce::Justification::centredBottom);

        // --- Needle -----------------------------------------------------
        const auto needleAngle = juce::degreesToRadians (angleDegreesForCorrelation (smoothedCorrelation));
        const auto needleLength = bounds.getHeight() * needleLengthFraction;

        juce::Path needle;
        needle.startNewSubPath (hubX, hubY);
        needle.lineTo (hubX + needleLength * std::sin (needleAngle),
                       hubY - needleLength * std::cos (needleAngle));

        g.setColour (BasilicaLookAndFeel::getMeterNeedleColour());
        g.strokePath (needle, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

        g.restoreState();
    }

    // Read-only text value interface exposing the current ballistic-smoothed
    // reading - the suite's AnalogMeter/HubNeedle A-07 pattern (JUCE 8.0.14
    // juce::AccessibilityTextValueInterface).
    class CorrelationMeter::ValueInterface final : public juce::AccessibilityTextValueInterface
    {
    public:
        explicit ValueInterface (const CorrelationMeter& ownerIn) noexcept : owner (ownerIn) {}

        bool isReadOnly() const override { return true; }

        juce::String getCurrentValueAsString() const override
        {
            // Signed two-decimal reading, e.g. "+0.85" / "0.00" / "-0.40".
            const auto value = owner.smoothedCorrelation;
            return (value > 0.0f ? "+" : "") + juce::String (value, 2);
        }

        void setValueAsString (const juce::String&) override {}

    private:
        const CorrelationMeter& owner;
    };

    std::unique_ptr<juce::AccessibilityHandler> CorrelationMeter::createAccessibilityHandler()
    {
        return std::make_unique<juce::AccessibilityHandler> (
            *this,
            juce::AccessibilityRole::label,
            juce::AccessibilityActions {},
            juce::AccessibilityHandler::Interfaces { std::make_unique<ValueInterface> (*this) });
    }
}
