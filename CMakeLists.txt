cmake_minimum_required(VERSION 3.24)

project(ACEAudioKeySync VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_binary_data(KeySyncAssets
    SOURCES
        assets/logo.png
        assets/fonts/Isometra-Regular.ttf
        assets/fonts/Inter-Regular.ttf
        assets/fonts/Inter-SemiBold.ttf)

set(KEYSYNC_SOURCES
    source/PluginProcessor.cpp
    source/PluginEditor.cpp
    source/dsp/KeyDetector.cpp
    source/dsp/Recommender.cpp)

foreach(edition FREE PRO)
    if(edition STREQUAL "FREE")
        set(plugin_target KeySyncFree)
        set(plugin_name "KeySync Free")
        set(plugin_code Ksyf)
        set(edition_flag KEYSYNC_EDITION_PRO=0)
    else()
        set(plugin_target KeySyncPro)
        set(plugin_name "KeySync Pro")
        set(plugin_code KsyP)
        set(edition_flag KEYSYNC_EDITION_PRO=1)
    endif()

    juce_add_plugin(${plugin_target}
        COMPANY_NAME "ACE Audio"
        BUNDLE_ID com.acea.KeySync${edition}
        PLUGIN_MANUFACTURER_CODE Acea
        PLUGIN_CODE ${plugin_code}
        FORMATS VST3
        PRODUCT_NAME "${plugin_name}"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_CATEGORIES "Fx")

    target_sources(${plugin_target} PRIVATE ${KEYSYNC_SOURCES})

    target_compile_definitions(${plugin_target} PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        ${edition_flag})

    target_link_libraries(${plugin_target} PRIVATE
        KeySyncAssets
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags)
endforeach()

# detector/recommender self-test (console)
add_executable(keysync_selftest
    source/SelfTest.cpp
    source/dsp/KeyDetector.cpp
    source/dsp/Recommender.cpp)
target_compile_definitions(keysync_selftest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(keysync_selftest PRIVATE juce::juce_audio_basics juce::juce_dsp)

# dev-only CLI probe: run the detector on real audio files for diagnosis
add_executable(keysync_probe
    source/TempoProbe.cpp
    source/dsp/KeyDetector.cpp
    source/dsp/Recommender.cpp)
target_compile_definitions(keysync_probe PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 KEY_SYNC_DEBUG_TEMPO=1 KEY_SYNC_DEBUG_KEY=1)
target_link_libraries(keysync_probe PRIVATE
    juce::juce_audio_basics juce::juce_dsp juce::juce_audio_formats)
