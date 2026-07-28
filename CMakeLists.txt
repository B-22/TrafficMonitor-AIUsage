cmake_minimum_required(VERSION 3.21)

set(CODEX_USAGE_VERSION "0.4.2" CACHE STRING "AIUsage plugin version")

project(AIUsage VERSION "${CODEX_USAGE_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use static MSVC runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/CodexUsageVersion.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/CodexUsageVersion.h"
    @ONLY
)

set(TRAFFICMONITOR_INCLUDE_DIR "" CACHE PATH "Directory containing TrafficMonitor PluginInterface.h")

if(NOT TRAFFICMONITOR_INCLUDE_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/PluginInterface.h")
        set(TRAFFICMONITOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/TrafficMonitor/include/PluginInterface.h")
        set(TRAFFICMONITOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/TrafficMonitor/include")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../TrafficMonitor/include/PluginInterface.h")
        set(TRAFFICMONITOR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../TrafficMonitor/include")
    else()
        message(FATAL_ERROR "PluginInterface.h not found. Set TRAFFICMONITOR_INCLUDE_DIR or place it in include/.")
    endif()
endif()

message(STATUS "PluginInterface.h dir: ${TRAFFICMONITOR_INCLUDE_DIR}")

# Static library: shared core (JSON, Codex parsing)
add_library(AIUsageCore STATIC
    src/CodexUsageCore.cpp
    src/JsonLite.cpp
)

target_include_directories(AIUsageCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_compile_definitions(AIUsageCore PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(AIUsageCore PRIVATE /utf-8)
endif()

# DLL: plugin + Claude fetcher + Codex fetcher + DPAPI + credential reader
add_library(AIUsage SHARED
    src/CodexUsagePlugin.cpp
    src/CodexUsageFetcher.cpp
    src/ClaudeUsageFetcher.cpp
    src/ClaudeCredentialReader.cpp
    src/DpapiHelper.cpp
    src/ProxyHelper.cpp
)

target_include_directories(AIUsage PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${TRAFFICMONITOR_INCLUDE_DIR}"
)

target_compile_definitions(AIUsage PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(AIUsage PRIVATE /utf-8)
endif()

target_link_libraries(AIUsage PRIVATE
    AIUsageCore
    shlwapi
    winhttp
    ws2_32
    crypt32
    bcrypt
    gdiplus
)

set_target_properties(AIUsage PROPERTIES
    PREFIX ""
    OUTPUT_NAME "AIUsagePreview"
)

# Tests
add_executable(CodexUsageTests
    tests/CodexUsageCoreTests.cpp
)

target_link_libraries(CodexUsageTests PRIVATE AIUsageCore)

if(MSVC)
    target_compile_options(CodexUsageTests PRIVATE /utf-8)
endif()

enable_testing()
add_test(NAME CodexUsageTests COMMAND CodexUsageTests)

add_executable(ProxyHelperTests
    tests/ProxyHelperTests.cpp
    src/ProxyHelper.cpp
)

target_include_directories(ProxyHelperTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(ProxyHelperTests PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(ProxyHelperTests PRIVATE
    winhttp
    ws2_32
)

if(MSVC)
    target_compile_options(ProxyHelperTests PRIVATE /utf-8)
endif()

add_test(NAME ProxyHelperTests COMMAND ProxyHelperTests)
