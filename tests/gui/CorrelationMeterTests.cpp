#include "gui/CorrelationMeter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

// The M3 vector correlation meter (issue #4): pure-function coverage of the
// ballistics and the correlation->angle mapping (both static so no timer or
// message loop is needed), plus the display-only component contract. Ported
// from Miserere's NeedleMeterTests (basilica-audio/miserere PR #31) and
// adapted to the [-1, +1] correlation domain.

TEST_CASE ("Ballistics step converges monotonically towards the target without overshoot", "[gui][meter]")
{
    using basilica::gui::CorrelationMeter;

    float smoothed = 0.0f;
    constexpr float target = 1.0f;
    constexpr float dt = 1.0f / 30.0f;

    float previous = smoothed;

    // Strict monotonicity only over the first ~1.3 s of the ramp - once the
    // gap shrinks towards float epsilon the per-tick increment legitimately
    // underflows to zero, so asserting strict `>` all the way to
    // convergence would test float rounding, not the ballistics.
    for (int i = 0; i < 40; ++i)
    {
        smoothed = CorrelationMeter::stepBallistics (previous, target, dt, CorrelationMeter::ballisticsTauSeconds);

        CHECK (smoothed > previous);  // strictly rising towards the target...
        CHECK (smoothed <= target);   // ...never past it
        previous = smoothed;
    }

    for (int i = 0; i < 400; ++i)
        smoothed = CorrelationMeter::stepBallistics (smoothed, target, dt, CorrelationMeter::ballisticsTauSeconds);

    // ~14.7 s total at 30 Hz >> tau (0.18 s): must have converged.
    CHECK (smoothed == Catch::Approx (target).margin (1.0e-3));

    // Falling back towards the opposite extreme is symmetric.
    for (int i = 0; i < 400; ++i)
        smoothed = CorrelationMeter::stepBallistics (smoothed, -1.0f, dt, CorrelationMeter::ballisticsTauSeconds);

    CHECK (smoothed == Catch::Approx (-1.0f).margin (1.0e-3));
}

TEST_CASE ("Ballistics edge cases: zero dt/tau jump straight to the target", "[gui][meter]")
{
    using basilica::gui::CorrelationMeter;

    CHECK (CorrelationMeter::stepBallistics (0.2f, 0.8f, 0.0f, 0.18f) == Catch::Approx (0.8f));
    CHECK (CorrelationMeter::stepBallistics (0.2f, 0.8f, -1.0f, 0.18f) == Catch::Approx (0.8f));
    CHECK (CorrelationMeter::stepBallistics (0.2f, 0.8f, 0.033f, 0.0f) == Catch::Approx (0.8f));
}

TEST_CASE ("Ballistics sanitise non-finite inputs instead of propagating them", "[gui][meter]")
{
    using basilica::gui::CorrelationMeter;
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN();
    constexpr auto inf = std::numeric_limits<float>::infinity();

    // A NaN/inf TARGET resolves to 0 (needle centred) - the step output
    // must stay finite whatever the engine hands over.
    CHECK (std::isfinite (CorrelationMeter::stepBallistics (0.5f, nan, 0.033f, 0.18f)));
    CHECK (std::isfinite (CorrelationMeter::stepBallistics (0.5f, inf, 0.033f, 0.18f)));
    CHECK (std::isfinite (CorrelationMeter::stepBallistics (0.5f, -inf, 0.033f, 0.18f)));

    // A poisoned CURRENT state recovers to the (sanitised) target instead
    // of sticking at NaN forever.
    CHECK (CorrelationMeter::stepBallistics (nan, 0.4f, 0.033f, 0.18f) == Catch::Approx (0.4f));
    CHECK (CorrelationMeter::stepBallistics (nan, nan, 0.033f, 0.18f) == Catch::Approx (0.0f));
}

TEST_CASE ("Correlation->angle mapping hits the engraved ticks exactly and clamps beyond both ends", "[gui][meter]")
{
    using basilica::gui::CorrelationMeter;

    // The tick table in CorrelationMeter.cpp: -1/-0.5/0/+0.5/+1 at
    // -50/-25/0/+25/+50 degrees (linear symmetric scale, +1 rests right).
    CHECK (CorrelationMeter::angleDegreesForCorrelation (-1.0f) == Catch::Approx (-50.0f));
    CHECK (CorrelationMeter::angleDegreesForCorrelation (-0.5f) == Catch::Approx (-25.0f));
    CHECK (CorrelationMeter::angleDegreesForCorrelation (0.0f) == Catch::Approx (0.0f));
    CHECK (CorrelationMeter::angleDegreesForCorrelation (0.5f) == Catch::Approx (25.0f));
    CHECK (CorrelationMeter::angleDegreesForCorrelation (1.0f) == Catch::Approx (50.0f));

    // Clamped beyond the scale (defensive - the engine already clamps).
    CHECK (CorrelationMeter::angleDegreesForCorrelation (-3.0f) == Catch::Approx (-50.0f));
    CHECK (CorrelationMeter::angleDegreesForCorrelation (3.0f) == Catch::Approx (50.0f));

    // Non-finite input centres the needle instead of producing a NaN angle.
    CHECK (CorrelationMeter::angleDegreesForCorrelation (std::numeric_limits<float>::quiet_NaN())
           == Catch::Approx (0.0f));

    // Strictly monotonically increasing across the live span - a more
    // correlated input always swings further right, no plateau or reversal.
    auto previous = CorrelationMeter::angleDegreesForCorrelation (-1.0f);

    for (float correlation = -0.95f; correlation <= 1.0f; correlation += 0.05f)
    {
        const auto angle = CorrelationMeter::angleDegreesForCorrelation (correlation);
        CHECK (angle > previous);
        previous = angle;
    }
}

TEST_CASE ("tick() follows the atomic target and never lets NaN reach the smoothed reading", "[gui][meter]")
{
    basilica::gui::CorrelationMeter meter ("Test correlation meter", "TEST");

    meter.setTargetCorrelation (0.8f);

    for (int i = 0; i < 300; ++i)
        meter.tick (1.0f / 30.0f);

    CHECK (meter.getSmoothedCorrelation() == Catch::Approx (0.8f).margin (1.0e-3));

    meter.setTargetCorrelation (std::numeric_limits<float>::quiet_NaN());

    for (int i = 0; i < 300; ++i)
        meter.tick (1.0f / 30.0f);

    CHECK (std::isfinite (meter.getSmoothedCorrelation()));
    CHECK (meter.getSmoothedCorrelation() == Catch::Approx (0.0f).margin (1.0e-3));
}

TEST_CASE ("CorrelationMeter is a display-only component with a titled, read-only accessible value", "[gui][meter][a11y]")
{
    basilica::gui::CorrelationMeter meter ("Input correlation meter", "IN");

    CHECK (meter.getTitle() == "Input correlation meter");
    CHECK_FALSE (meter.getWantsKeyboardFocus());

    bool clicksSelf = true, clicksChildren = true;
    meter.getInterceptsMouseClicks (clicksSelf, clicksChildren);
    CHECK_FALSE (clicksSelf);
    CHECK_FALSE (clicksChildren);

    // createAccessibilityHandler() (not getAccessibilityHandler()) - the
    // latter needs a live native peer (JUCE 8.0.14 juce_Component.cpp:
    // 3323-3326), which this headless binary never has.
    const auto handler = meter.createAccessibilityHandler();
    REQUIRE (handler != nullptr);

    auto* valueInterface = handler->getValueInterface();
    REQUIRE (valueInterface != nullptr);
    CHECK (valueInterface->isReadOnly());
    CHECK (valueInterface->getCurrentValueAsString() == "0.00");

    // Positive readings carry an explicit sign, negatives keep their own.
    meter.setImmediateCorrelationForPreview (0.85f);
    CHECK (valueInterface->getCurrentValueAsString() == "+0.85");

    meter.setImmediateCorrelationForPreview (-0.4f);
    CHECK (valueInterface->getCurrentValueAsString() == "-0.40");
}
