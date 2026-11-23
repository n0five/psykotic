cmake_minimum_required(VERSION 3.22)
project(PsykoticDist VERSION 0.1.0)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(PsykoticDist
    COMPANY_NAME "Perso"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    PLUGIN_MANUFACTURER_CODE Pers
    PLUGIN_CODE Psyk
    FORMATS VST3 Standalone
    PRODUCT_NAME "PSYKOTIC Dist"
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(PsykoticDist PRIVATE PluginProcessor.cpp)

target_compile_definitions(PsykoticDist PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(PsykoticDist
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lib_flags
        juce::juce_recommended_warning_flags)
