#include "gui/ArcNeedleMeter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

// Component tests for the D4 correlation arc instrument's numeric core
// (gui/ArcNeedleMeter.h): the correlation -> angle mapping, the spring
// ballistics, and the accessibility surface. The visual/needle-motion
// proofs live in EditorSnapshotTests.cpp against the real editor tree.

TEST_CASE ("Correlation->angle mapping is linear, centre-zero, and clamps beyond both ends", "[gui][meter]")
{
    using M = basilica::gui::ArcNeedleMeter;

    CHECK (M::angleDegreesForCorrelation (0.0f) == Catch::Approx (0.0f));
    CHECK (M::angleDegreesForCorrelation (1.0f) == Catch::Approx (M::arcEndAngleDeg));
    CHECK (M::angleDegreesForCorrelation (-1.0f) == Catch::Approx (-M::arcEndAngleDeg));
    CHECK (M::angleDegreesForCorrelation (0.5f) == Catch::Approx (0.5f * M::arcEndAngleDeg));

    // Out-of-range estimates (transient FFT edge cases) pin at the stops.
    CHECK (M::angleDegreesForCorrelation (3.0f) == Catch::Approx (M::arcEndAngleDeg));
    CHECK (M::angleDegreesForCorrelation (-3.0f) == Catch::Approx (-M::arcEndAngleDeg));
}

TEST_CASE ("Arc meter ballistics converge monotonically towards the target without overshoot", "[gui][meter]")
{
    using M = basilica::gui::ArcNeedleMeter;

    float value = -1.0f;
    float previous = value;

    for (int i = 0; i < 200; ++i)
    {
        value = M::stepBallistics (value, 1.0f, 1.0f / 30.0f, M::ballisticsTauSeconds);
        CHECK (value >= previous);
        CHECK (value <= 1.0f);
        previous = value;
    }

    CHECK (value == Catch::Approx (1.0f).margin (0.01));
}

TEST_CASE ("Arc meter ballistics edge cases: zero dt/tau jump straight to the target", "[gui][meter]")
{
    using M = basilica::gui::ArcNeedleMeter;

    CHECK (M::stepBallistics (0.2f, 0.9f, 0.0f, 0.3f) == Catch::Approx (0.9f));
    CHECK (M::stepBallistics (0.2f, 0.9f, 0.033f, 0.0f) == Catch::Approx (0.9f));
}

TEST_CASE ("Arc meter ballistics sanitise non-finite targets instead of propagating them", "[gui][meter]")
{
    using M = basilica::gui::ArcNeedleMeter;

    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto inf = std::numeric_limits<float>::infinity();

    CHECK (M::stepBallistics (0.4f, nan, 0.033f, 0.3f) == Catch::Approx (0.4f));
    CHECK (M::stepBallistics (0.4f, inf, 0.033f, 0.3f) == Catch::Approx (0.4f));
}

TEST_CASE ("ArcNeedleMeter is display-only with a titled, read-only accessible value", "[gui][meter][a11y]")
{
    basilica::gui::ArcNeedleMeter meter (juce::Image {}, "Output Correlation meter", 0.5f, 0.7f);

    CHECK (meter.getTitle() == "Output Correlation meter");
    CHECK_FALSE (meter.getWantsKeyboardFocus());

    const auto handler = meter.createAccessibilityHandler();
    REQUIRE (handler != nullptr);

    auto* valueInterface = handler->getValueInterface();
    REQUIRE (valueInterface != nullptr);
    CHECK (valueInterface->isReadOnly());

    meter.setImmediateCorrelationForPreview (-0.25f);
    CHECK (valueInterface->getCurrentValue() == Catch::Approx (-0.25));
    CHECK (valueInterface->getCurrentValueAsString().contains ("-0.25"));
}
