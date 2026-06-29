cmake_minimum_required(VERSION 3.20)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(moonbase_cpp
    # VERSION is managed by semantic-release; do not edit by hand.
    VERSION 3.3.0
    DESCRIPTION "Header-only C++ SDK for Moonbase license activation"
    LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(MOONBASE_IS_TOP_LEVEL ON)
else()
    set(MOONBASE_IS_TOP_LEVEL OFF)
endif()

option(MOONBASE_BUILD_TESTS "Build Moonbase C++ SDK tests" ${MOONBASE_IS_TOP_LEVEL})
option(MOONBASE_BUILD_EXAMPLES "Build Moonbase C++ SDK examples" ${MOONBASE_IS_TOP_LEVEL})
option(MOONBASE_BUILD_JUCE_EXAMPLE
    "Build the JUCE bridge example (fetches JUCE; off by default)" OFF)
option(MOONBASE_BUILD_JUCE_NATIVE_EXAMPLE
    "Build the native JUCE module sample app (fetches JUCE; off by default)" OFF)
option(MOONBASE_BUILD_UI_SNAPSHOTS
    "Build the UI snapshot harness for visual tests (fetches JUCE; off by default)" OFF)
option(MOONBASE_BUILD_JUCE_TESTS
    "Build the JUCE module unit tests (fetches JUCE + doctest; off by default)" OFF)

# Optional sanitizer for the test targets. One of: address | thread | undefined
# | address,undefined (ASan and TSan can't be combined). Empty = off, so normal
# builds are unaffected. Example:
#   cmake -B build-tsan -DMOONBASE_BUILD_JUCE_TESTS=ON -DMOONBASE_SANITIZER=thread
set(MOONBASE_SANITIZER "" CACHE STRING
    "Sanitizer for test targets: address | thread | undefined | address,undefined")

function(moonbase_apply_sanitizer target)
    if(MOONBASE_SANITIZER AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=${MOONBASE_SANITIZER} -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=${MOONBASE_SANITIZER})
    endif()
endfunction()

# The libcurl HTTP transport is the SDK's default. The JUCE module ships its own
# JUCE-based transport and needs no curl, so JUCE-only builds (the module + its
# tests) can turn this off and avoid the dependency entirely.
option(MOONBASE_USE_CURL "Build the SDK target with the libcurl HTTP transport" ON)

if(MOONBASE_USE_CURL)
    find_package(CURL REQUIRED)
endif()
find_package(OpenSSL REQUIRED)

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
    set(MOONBASE_NLOHMANN_FETCHED ON)
endif()

add_library(moonbase_cpp INTERFACE)
add_library(moonbase::licensing ALIAS moonbase_cpp)
set_target_properties(moonbase_cpp PROPERTIES EXPORT_NAME licensing)

target_compile_features(moonbase_cpp INTERFACE cxx_std_17)
target_include_directories(moonbase_cpp
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(moonbase_cpp
    INTERFACE
        OpenSSL::SSL
        OpenSSL::Crypto)
if(MOONBASE_USE_CURL)
    target_link_libraries(moonbase_cpp INTERFACE CURL::libcurl)
else()
    target_compile_definitions(moonbase_cpp INTERFACE MOONBASE_DISABLE_CURL_TRANSPORT=1)
endif()
if(MOONBASE_NLOHMANN_FETCHED)
    target_include_directories(moonbase_cpp
        INTERFACE
            $<BUILD_INTERFACE:${nlohmann_json_SOURCE_DIR}/include>)
else()
    target_link_libraries(moonbase_cpp
        INTERFACE
            nlohmann_json::nlohmann_json)
endif()
target_compile_definitions(moonbase_cpp
    INTERFACE
        MOONBASE_CPP_VERSION="${PROJECT_VERSION}")

if(MOONBASE_BUILD_EXAMPLES)
    add_executable(moonbase_activation_example examples/activation.cpp)
    target_link_libraries(moonbase_activation_example PRIVATE moonbase::licensing)
endif()

if(MOONBASE_BUILD_JUCE_EXAMPLE)
    enable_language(C)
    if(APPLE)
        enable_language(OBJC OBJCXX)
    endif()
    add_subdirectory(examples/juce)
endif()

# The native sample app and the UI snapshot harness both consume the JUCE module.
# Fetch JUCE and add the module once here so the two targets can share them
# (juce_add_module on the same folder twice would collide).
if(MOONBASE_BUILD_JUCE_NATIVE_EXAMPLE OR MOONBASE_BUILD_UI_SNAPSHOTS OR MOONBASE_BUILD_JUCE_TESTS)
    enable_language(C)
    if(APPLE)
        enable_language(OBJC OBJCXX)
    endif()

    include(FetchContent)
    set(MOONBASE_JUCE_VERSION "8.0.4" CACHE STRING "JUCE tag to fetch for the JUCE module targets")
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${MOONBASE_JUCE_VERSION}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)

    if(NOT TARGET moonbase_licensing)
        juce_add_module(${CMAKE_CURRENT_SOURCE_DIR}/modules/moonbase_licensing)
    endif()
endif()

if(MOONBASE_BUILD_JUCE_NATIVE_EXAMPLE)
    add_subdirectory(examples/juce-native)
endif()

if(MOONBASE_BUILD_UI_SNAPSHOTS)
    add_subdirectory(tests/visual)
endif()

if(MOONBASE_BUILD_TESTS)
    enable_testing()

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

    add_executable(moonbase_tests
        tests/main.cpp
        tests/client_tests.cpp
        tests/fingerprint_tests.cpp
        tests/header_smoke_tests.cpp
        tests/inventory_tests.cpp
        tests/licensing_tests.cpp
        tests/process_dedup_tests.cpp
        tests/store_tests.cpp
        tests/validator_tests.cpp
        tests/version_tests.cpp
        tests/live_tests.cpp)
    target_link_libraries(moonbase_tests PRIVATE moonbase::licensing doctest::doctest)
    moonbase_apply_sanitizer(moonbase_tests)
    include(doctest)
    doctest_discover_tests(moonbase_tests)
endif()

# JUCE module unit tests (doctest, links the moonbase_licensing JUCE module).
# Placed after the SDK-test block so doctest is already available when both
# MOONBASE_BUILD_TESTS and MOONBASE_BUILD_JUCE_TESTS are enabled together.
if(MOONBASE_BUILD_JUCE_TESTS)
    enable_testing()
    add_subdirectory(tests/juce)
endif()

install(TARGETS moonbase_cpp
    EXPORT moonbase_cppTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")

write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/moonbase_cppConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
configure_package_config_file(
    cmake/moonbase_cppConfig.cmake.in
    "${PROJECT_BINARY_DIR}/moonbase_cppConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/moonbase_cpp)

install(EXPORT moonbase_cppTargets
    FILE moonbase_cppTargets.cmake
    NAMESPACE moonbase::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/moonbase_cpp)
install(FILES
    "${PROJECT_BINARY_DIR}/moonbase_cppConfig.cmake"
    "${PROJECT_BINARY_DIR}/moonbase_cppConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/moonbase_cpp)
