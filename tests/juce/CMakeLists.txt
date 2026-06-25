# JUCE module unit tests. JUCE + the moonbase_licensing module are provided by
# the top-level CMakeLists. Opt in with:
#     cmake -B build -DMOONBASE_BUILD_JUCE_TESTS=ON
#     cmake --build build --target MoonbaseJuceTests
#     ctest --test-dir build -R Juce --output-on-failure

# doctest may already be available from the SDK-test block; fetch it otherwise.
if(NOT TARGET doctest::doctest)
    find_package(doctest QUIET)
    if(NOT doctest_FOUND)
        include(FetchContent)
        set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
        FetchContent_Declare(
            doctest
            URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(doctest)
        list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
    endif()
endif()

juce_add_console_app(MoonbaseJuceTests
    PRODUCT_NAME "Moonbase JUCE Tests")

target_sources(MoonbaseJuceTests PRIVATE controller_tests.cpp)

target_compile_features(MoonbaseJuceTests PRIVATE cxx_std_17)

target_compile_definitions(MoonbaseJuceTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_MODAL_LOOPS_PERMITTED=1  # exposes MessageManager::runDispatchLoopUntil for pumping
    JUCE_APPLICATION_NAME_STRING="Moonbase JUCE Tests"
    JUCE_APPLICATION_VERSION_STRING="1.0.0")

# test_helpers.hpp (shared with the SDK tests) lives one directory up and signs
# tokens with OpenSSL. The moonbase SDK headers come from the module itself.
target_include_directories(MoonbaseJuceTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(MoonbaseJuceTests PRIVATE
    moonbase_licensing
    doctest::doctest
    OpenSSL::SSL
    OpenSSL::Crypto
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

moonbase_apply_sanitizer(MoonbaseJuceTests)

include(doctest)
doctest_discover_tests(MoonbaseJuceTests
    TEST_PREFIX "Juce.")
