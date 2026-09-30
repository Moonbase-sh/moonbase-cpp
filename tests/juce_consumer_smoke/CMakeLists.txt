# A JUCE project consuming this repository the standard way, with
# MOONBASE_JUCE_MODULE_ONLY on. The JUCE counterpart of tests/consumer_smoke.
#
# The module needs neither OpenSSL nor CURL, so CI configures this with both
# lookups disabled:
#     cmake -S tests/juce_consumer_smoke -B build-juce-consumer \
#         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON -DCMAKE_DISABLE_FIND_PACKAGE_CURL=ON
#     cmake --build build-juce-consumer --target MoonbaseJuceConsumerSmoke
#
# Any find_package(OpenSSL REQUIRED) or find_package(CURL REQUIRED) left on the
# module-only path then fails the configure.

cmake_minimum_required(VERSION 3.22)

project(moonbase_juce_consumer_smoke LANGUAGES C CXX)

include(FetchContent)
set(MOONBASE_JUCE_VERSION "8.0.4" CACHE STRING "JUCE tag to fetch")
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ${MOONBASE_JUCE_VERSION}
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# What FetchContent_MakeAvailable(moonbase_cpp) does with a fetched checkout.
set(MOONBASE_JUCE_MODULE_ONLY ON)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. moonbase_cpp)

juce_add_console_app(MoonbaseJuceConsumerSmoke
    PRODUCT_NAME "Moonbase JUCE Consumer Smoke"
    VERSION "1.0.0")

target_sources(MoonbaseJuceConsumerSmoke PRIVATE main.cpp)

target_compile_features(MoonbaseJuceConsumerSmoke PRIVATE cxx_std_17)

target_compile_definitions(MoonbaseJuceConsumerSmoke PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(MoonbaseJuceConsumerSmoke PRIVATE
    moonbase_licensing
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
