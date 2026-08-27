cmake_minimum_required(VERSION 3.24)

# Must be set before the first project() call to take effect.
# CI overrides CMAKE_OSX_ARCHITECTURES explicitly for release/universal builds;
# local builds are left at the native architecture.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment target" FORCE)

# JUCE's build tooling (juceaide) and some CMake find-modules it triggers
# (e.g. FindThreads) expect the C toolchain to be available; declaring it
# here at the top level avoids it being enabled implicitly deeper inside
# JUCE's CMake helpers, which has been observed to break Ninja's generate
# step (missing CMAKE_C_COMPILE_OBJECT rule variable).
project(Firmament VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/CPM.cmake)

# CPMAddPackage automatically respects the CPM_SOURCE_CACHE environment
# variable if it is set, reusing a shared package cache across builds/checkouts.
if(DEFINED ENV{CPM_SOURCE_CACHE})
    message(STATUS "Firmament: using CPM_SOURCE_CACHE at $ENV{CPM_SOURCE_CACHE}")
endif()

CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG 8.0.14
    GIT_SHALLOW TRUE
)

# ==============================================================================
# Plugin target
# ==============================================================================

juce_add_plugin(Firmament
    # The trading name, not the legal person. This is the string a host groups
    # plugins by - Logic's plugin manager, Cubase's vendor column, Reaper's FX
    # browser - and the suite ships under one brand. Display only: the VST3
    # class ID derives from PLUGIN_MANUFACTURER_CODE + PLUGIN_CODE alone (JUCE
    # 8.0.14, juce_VST3ModuleInfo.h, VST3Interface::jucePluginId) and the Audio
    # Unit identity triple stays (aufx, Firmament's PLUGIN_CODE, Yvsv), so no
    # existing session is disturbed. See docs/branding.md.
    COMPANY_NAME "Basilica Audio"
    # DELIBERATELY NOT RENAMED to com.basilicaaudio.*, and not an oversight.
    # The bundle ID is how a host recognises this plugin across sessions, and
    # how every preset file ever written names it ("plugin" field). Changing it
    # makes every existing project treat this as a different plugin, for no
    # user-visible benefit - nothing a customer sees flows from it. Recorded in
    # basilica-audio/.github docs/adr/0001-suite-vendor-identity.md.
    BUNDLE_ID com.yvesvogl.firmament
    PLUGIN_MANUFACTURER_CODE Yvsv
    PLUGIN_CODE Frmm
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "Firmament"
    COMPANY_WEBSITE "https://basilica-audio.github.io/website/"
    # Names the legal person, not the trading name above, and stays that way:
    # Basilica Audio is how the suite is sold, Yves Vogl is who holds the
    # copyright. Aligning the two would make this notice less accurate.
    COMPANY_COPYRIGHT "Copyright (c) 2026 Yves Vogl"
    DESCRIPTION "Mid/side stereo widener and imager with bass-mono"
    VST3_CATEGORIES Fx Spatial Stereo
    # Relative paths are resolved against this CMakeLists.txt's directory and
    # validated at configure time (JUCE 8.0.14, JUCEModuleSupport.cmake's
    # _juce_make_absolute_and_check).
    ICON_BIG "docs/assets/icon.png"
    COPY_PLUGIN_AFTER_BUILD TRUE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
)

# ==============================================================================
# Embedded assets: M2 preset system factory presets (presets/factory/*.json)
# and the M2 i18n frame's German translation (resources/i18n/de.txt). See
# src/presets/PresetManager.h and src/presets/Localisation.h - both consume
# these purely via BinaryData:: symbols passed in from PluginProcessor.cpp/
# PluginEditor.cpp, never by including BinaryData.h themselves, so they stay
# portable across the suite (basilica-audio/nave's docs/preset-system-notes.md
# is the replication recipe this was copied from).
# ==============================================================================

juce_add_binary_data(FirmamentBinaryData SOURCES
    presets/factory/default.json
    presets/factory/openStrings.json
    presets/factory/choirBloom.json
    presets/factory/doubledRhythmGlue.json
    presets/factory/masterBusBassMono.json
    presets/factory/automatedWidthSafetyNet.json
    presets/factory/monoSafeAir.json
    presets/factory/widePadFullPrecedence.json
    presets/factory/extremeWidth.json
    presets/factory/subtleOpenness.json
    presets/factory/velvetWidth.json
    presets/factory/masteringLinearPhaseBassMono.json
    presets/factory/threeBandImager.json
    resources/i18n/de.txt
    # Suite typography (M3 vector editor, issue #4): EB Garamond, OFL
    # licensed (resources/fonts/OFL-EBGaramond.txt) - consumed by
    # src/gui/BasilicaLookAndFeel.cpp as the single source of every glyph
    # the fully JUCE-drawn editor renders.
    resources/fonts/EBGaramond-Regular.ttf
    resources/fonts/EBGaramond-SemiBold.ttf
    # Wave-3 compositional GUI (campaign 2026-08, supersedes the M3 vector
    # editor's runtime-drawn visuals - those components stay in the tree per
    # the suite's "superseded, not deleted" convention):
    #   - plate_firmament.png: the accepted EMPTY family plate render
    #     (.scaffold/gui-assets/rollout-2026-07/firmament/out/plate-01.png -
    #     see out/VERDICT.md).
    #   - sprite_*.png: extracted control sprites from
    #     .scaffold/gui-assets/sprite-library/ (see its provenance.md for
    #     source render + crop rect per sprite), backing-blended via
    #     blend_sprite_backing.py so their basalt patches cross-fade into
    #     the local plate ground (no visible rectangular patches).
    #   - sprite_needle_master05.png: master-diff-extracted needle
    #     (NADEL-REGEL; provenance JSON alongside the PNG).
    #   - layout_manifest.json: the compositional layout manifest, the
    #     single source of truth for control positions (see
    #     src/gui/LayoutManifest.h).
    resources/gui/plate_firmament.png
    resources/gui/sprite_knob_brass.png
    resources/gui/sprite_selector_stepped.png
    resources/gui/sprite_toggle_up.png
    resources/gui/sprite_arc_meter.png
    resources/gui/sprite_needle_master05.png
    resources/gui/layout_manifest.json
)

# ==============================================================================
# SharedCode: an INTERFACE target carrying the plugin's sources, definitions,
# and library links so that both the plugin targets (AU/VST3/Standalone) and
# the Tests executable compile the exact same code (Pamplejuce pattern, see
# https://github.com/sudara/pamplejuce), avoiding ODR violations between them.
# ==============================================================================

add_library(SharedCode INTERFACE)

target_compile_features(SharedCode INTERFACE cxx_std_20)

file(GLOB_RECURSE FirmamentSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

target_sources(SharedCode INTERFACE ${FirmamentSources})

target_include_directories(SharedCode INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(SharedCode INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # AGPLv3 usage permits disabling the splash screen.
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # processBlock() holds a juce::ScopedNoDenormals, which forces FTZ/DAZ for
    # the whole audio callback, so juce_dsp's per-block snap-to-zero passes are
    # redundant work on the audio thread. juce_dsp.h (JUCE 8.0.14) documents
    # disabling the flag as safe exactly in this case. It also keeps denormal
    # tail tests honest: the library no longer masks a missing FTZ path.
    # Fleet decision: basilica-audio/Crypta#99.
    JUCE_DSP_ENABLE_SNAP_TO_ZERO=0
)

# Add any extra juce:: modules a specific plugin needs (e.g. juce_audio_formats
# for reading impulse-response files) to this list.
target_link_libraries(SharedCode INTERFACE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
    FirmamentBinaryData
)

target_link_libraries(Firmament PRIVATE SharedCode)

# ==============================================================================
# Tests
# ==============================================================================

enable_testing()
set(CTEST_OUTPUT_ON_FAILURE ON)

CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.15.2
)

file(GLOB_RECURSE FirmamentTestSources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h")

add_executable(Tests ${FirmamentTestSources})

target_compile_features(Tests PRIVATE cxx_std_20)

target_link_libraries(Tests PRIVATE SharedCode Catch2::Catch2)

# Copy over the plugin target's compile definitions (JucePlugin_* macros etc.)
# so plugin code compiles identically when built as part of the Tests binary.
target_compile_definitions(Tests PRIVATE
    $<TARGET_PROPERTY:Firmament,COMPILE_DEFINITIONS>
    JUCE_MODAL_LOOPS_PERMITTED=1
    # Absolute path to docs/, so tests/gui/EditorSnapshotTests.cpp can write
    # the committed docs/gui-preview.png directly rather than a copy step.
    FIRMAMENT_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs"
)

set_target_properties(Tests PROPERTIES XCODE_GENERATE_SCHEME ON)

include(${Catch2_SOURCE_DIR}/extras/Catch.cmake)

if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(FirmamentDiscoveryMode PRE_TEST)
else()
    set(FirmamentDiscoveryMode POST_BUILD)
endif()

catch_discover_tests(Tests DISCOVERY_MODE ${FirmamentDiscoveryMode})
