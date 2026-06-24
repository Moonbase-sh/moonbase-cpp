# Offscreen UI snapshot harness. JUCE + the moonbase_licensing module are
# provided by the top-level CMakeLists. Opt in with:
#     cmake -B build -DMOONBASE_BUILD_UI_SNAPSHOTS=ON
#     cmake --build build --target MoonbaseUiSnapshots
#     ./<build>/tests/visual/MoonbaseUiSnapshots ui-snapshots

juce_add_console_app(MoonbaseUiSnapshots
    PRODUCT_NAME "Moonbase UI Snapshots")

target_sources(MoonbaseUiSnapshots PRIVATE snapshot_main.cpp)

target_compile_features(MoonbaseUiSnapshots PRIVATE cxx_std_17)

target_compile_definitions(MoonbaseUiSnapshots PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_APPLICATION_NAME_STRING="Moonbase UI Snapshots"
    JUCE_APPLICATION_VERSION_STRING="1.0.0")

target_link_libraries(MoonbaseUiSnapshots PRIVATE
    moonbase_licensing
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
