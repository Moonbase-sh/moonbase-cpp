cmake_minimum_required(VERSION 3.20)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(moonbase_cpp
    # VERSION is managed by semantic-release; do not edit by hand.
    VERSION 2.1.0
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

find_package(CURL REQUIRED)
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

add_library(moonbase_licensing INTERFACE)
add_library(moonbase::licensing ALIAS moonbase_licensing)
set_target_properties(moonbase_licensing PROPERTIES EXPORT_NAME licensing)

target_compile_features(moonbase_licensing INTERFACE cxx_std_17)
target_include_directories(moonbase_licensing
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(moonbase_licensing
    INTERFACE
        CURL::libcurl
        OpenSSL::SSL
        OpenSSL::Crypto)
if(MOONBASE_NLOHMANN_FETCHED)
    target_include_directories(moonbase_licensing
        INTERFACE
            $<BUILD_INTERFACE:${nlohmann_json_SOURCE_DIR}/include>)
else()
    target_link_libraries(moonbase_licensing
        INTERFACE
            nlohmann_json::nlohmann_json)
endif()
target_compile_definitions(moonbase_licensing
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
        tests/licensing_tests.cpp
        tests/store_tests.cpp
        tests/validator_tests.cpp
        tests/live_tests.cpp)
    target_link_libraries(moonbase_tests PRIVATE moonbase::licensing doctest::doctest)
    include(doctest)
    doctest_discover_tests(moonbase_tests)
endif()

install(TARGETS moonbase_licensing
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
