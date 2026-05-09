# Standalone JUCE example app for the Moonbase activation SDK.
#
# Opt in from the top-level build with:
#     cmake -B build -DMOONBASE_BUILD_JUCE_EXAMPLE=ON
#     cmake --build build --target MoonbaseJuceExample
#
# This pulls JUCE via FetchContent on first configure (~80 MB; the initial
# build also takes several minutes while JUCE compiles). Subsequent builds
# reuse the populated source and binaries.

include(FetchContent)

set(MOONBASE_JUCE_VERSION "8.0.4" CACHE STRING
    "JUCE tag to fetch for the Moonbase JUCE example")

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ${MOONBASE_JUCE_VERSION}
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(MoonbaseJuceExample
    PRODUCT_NAME "Moonbase JUCE Example"
    COMPANY_NAME "Moonbase"
    BUNDLE_ID "sh.moonbase.cpp.juce-example"
    VERSION "1.0.0")

target_sources(MoonbaseJuceExample PRIVATE
    Main.cpp)

target_compile_features(MoonbaseJuceExample PRIVATE cxx_std_17)

target_compile_definitions(MoonbaseJuceExample PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MoonbaseJuceExample,JUCE_PROJECT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MoonbaseJuceExample,JUCE_VERSION>")

target_link_libraries(MoonbaseJuceExample PRIVATE
    moonbase::licensing
    juce::juce_gui_extra
    juce::juce_product_unlocking
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
