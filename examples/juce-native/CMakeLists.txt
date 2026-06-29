# Standalone sample app that uses the moonbase_licensing JUCE module.
#
# JUCE is fetched and the module is added by the top-level CMakeLists (shared
# with the UI snapshot target). Opt in with:
#     cmake -B build -DMOONBASE_BUILD_JUCE_NATIVE_EXAMPLE=ON
#     cmake --build build --target MoonbaseActivationNative

juce_add_gui_app(MoonbaseActivationNative
    PRODUCT_NAME "Moonbase Activation"
    COMPANY_NAME "Moonbase"
    BUNDLE_ID "sh.moonbase.activation.native"
    VERSION "0.0.1")

target_sources(MoonbaseActivationNative PRIVATE Main.cpp)

target_compile_features(MoonbaseActivationNative PRIVATE cxx_std_17)

target_compile_definitions(MoonbaseActivationNative PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MoonbaseActivationNative,JUCE_PROJECT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MoonbaseActivationNative,JUCE_VERSION>")

target_link_libraries(MoonbaseActivationNative PRIVATE
    moonbase_licensing
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
