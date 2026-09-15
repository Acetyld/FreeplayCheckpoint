cmake_minimum_required(VERSION 3.24)

# FreeplayCheckpoint is a BakkesMod plugin. BakkesMod and the supplied SDK
# libraries are Windows-only, so fail early with a useful message elsewhere.
if(NOT WIN32)
    message(FATAL_ERROR
        "FreeplayCheckpoint can only be built on Windows because BakkesMod "
        "and pluginsdk.lib are Windows-only."
    )
endif()

project(CheckpointPlugin LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# Build requirements
# ---------------------------------------------------------------------------

# The upstream Visual Studio project builds against the MSVC ABI and links
# BakkesMod's pluginsdk.lib. Keep CMake builds on the same ABI.
if(NOT MSVC)
    message(FATAL_ERROR
        "FreeplayCheckpoint requires the MSVC toolchain. "
        "In CLion, select a Visual Studio/MSVC toolchain rather than MinGW."
    )
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "FreeplayCheckpoint must be built for x64. "
        "Configure CMake with a 64-bit MSVC toolchain (for example -A x64)."
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match the upstream .vcxproj:
#   Release -> /MT
#   Debug   -> /MTd
set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# ---------------------------------------------------------------------------
# BakkesMod SDK discovery
# ---------------------------------------------------------------------------

# Users/CI may override either of these:
#
#   cmake -S . -B build -A x64 ^
#       -DBAKKESMOD_PATH="C:/path/to/bakkesmod"
#
# or:
#
#   cmake -S . -B build -A x64 ^
#       -DBAKKESMOD_SDK_PATH="C:/path/to/bakkesmodsdk"
#
set(BAKKESMOD_PATH "" CACHE PATH
    "BakkesMod root directory (the directory containing bakkesmodsdk and plugins)"
)
set(BAKKESMOD_SDK_PATH "" CACHE PATH
    "BakkesMod SDK directory (the directory containing include, lib, and bakkesmod-patch.exe)"
)

# 1. Explicit CMake cache value wins.
# 2. Then BAKKESMOD_PATH environment variable.
# 3. Then the same registry value used by the upstream BakkesMod.props.
# 4. Finally try the standard AppData installation path.
if(NOT BAKKESMOD_PATH AND DEFINED ENV{BAKKESMOD_PATH}
   AND NOT "$ENV{BAKKESMOD_PATH}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{BAKKESMOD_PATH}" _bakkesmod_env_path)
    set(BAKKESMOD_PATH "${_bakkesmod_env_path}" CACHE PATH
        "BakkesMod root directory" FORCE
    )
endif()

if(NOT BAKKESMOD_PATH AND NOT BAKKESMOD_SDK_PATH)
    cmake_host_system_information(
        RESULT _bakkesmod_registry_path
        QUERY WINDOWS_REGISTRY "HKCU/Software/BakkesMod/AppPath"
        VALUE "BakkesModPath"
        VIEW HOST
        ERROR_VARIABLE _bakkesmod_registry_error
    )

    if(NOT _bakkesmod_registry_error
       AND NOT "${_bakkesmod_registry_path}" STREQUAL "")
        file(TO_CMAKE_PATH
            "${_bakkesmod_registry_path}"
            _bakkesmod_registry_path_normalized
        )
        set(BAKKESMOD_PATH
            "${_bakkesmod_registry_path_normalized}"
            CACHE PATH "BakkesMod root directory" FORCE
        )
    endif()
endif()

if(NOT BAKKESMOD_PATH AND NOT BAKKESMOD_SDK_PATH
   AND DEFINED ENV{APPDATA})
    file(TO_CMAKE_PATH
        "$ENV{APPDATA}/bakkesmod/bakkesmod"
        _bakkesmod_default_path
    )

    if(IS_DIRECTORY "${_bakkesmod_default_path}")
        set(BAKKESMOD_PATH
            "${_bakkesmod_default_path}"
            CACHE PATH "BakkesMod root directory" FORCE
        )
    endif()
endif()

if(NOT BAKKESMOD_SDK_PATH AND BAKKESMOD_PATH)
    set(BAKKESMOD_SDK_PATH
        "${BAKKESMOD_PATH}/bakkesmodsdk"
        CACHE PATH "BakkesMod SDK directory" FORCE
    )
endif()

if(NOT BAKKESMOD_SDK_PATH)
    message(FATAL_ERROR
        "Could not locate the BakkesMod SDK.\n"
        "Install BakkesMod, or configure with one of:\n"
        "  -DBAKKESMOD_PATH=\"C:/path/to/bakkesmod\"\n"
        "  -DBAKKESMOD_SDK_PATH=\"C:/path/to/bakkesmodsdk\""
    )
endif()

# Normalize paths for CMake and make diagnostics easier to read.
file(TO_CMAKE_PATH "${BAKKESMOD_SDK_PATH}" BAKKESMOD_SDK_PATH)

set(_bakkesmod_include_dir "${BAKKESMOD_SDK_PATH}/include")
set(_bakkesmod_lib_dir     "${BAKKESMOD_SDK_PATH}/lib")

if(NOT IS_DIRECTORY "${_bakkesmod_include_dir}")
    message(FATAL_ERROR
        "BakkesMod SDK include directory does not exist:\n"
        "  ${_bakkesmod_include_dir}"
    )
endif()

find_library(
    BAKKESMOD_PLUGINSDK
    NAMES pluginsdk
    PATHS "${_bakkesmod_lib_dir}"
    NO_DEFAULT_PATH
    REQUIRED
)

find_program(
    BAKKESMOD_PATCHER
    NAMES bakkesmod-patch.exe
    PATHS "${BAKKESMOD_SDK_PATH}"
    NO_DEFAULT_PATH
    REQUIRED
)

message(STATUS "BakkesMod SDK: ${BAKKESMOD_SDK_PATH}")
message(STATUS "BakkesMod pluginsdk: ${BAKKESMOD_PLUGINSDK}")
message(STATUS "BakkesMod patcher: ${BAKKESMOD_PATCHER}")

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------

set(FREEPLAYCHECKPOINT_SOURCES
        CheckpointPlugin.cpp
        SettingsFile.cpp
        bindings.cpp
        geometry.cpp
        state.cpp
        presets.cpp
        PresetImportUI.cpp
        pch.cpp

        # Dear ImGui
        IMGUI/imgui.cpp
        IMGUI/imgui_draw.cpp
        IMGUI/imgui_widgets.cpp

        fmt/src/format.cc
        fmt/src/os.cc

        CheckpointPlugin.rc
)

set(FREEPLAYCHECKPOINT_HEADERS
    CheckpointPlugin.h
    state.h
    pch.h
    version.h
        PresetImportUI.cpp
)

add_library(CheckpointPlugin SHARED
    ${FREEPLAYCHECKPOINT_SOURCES}
    ${FREEPLAYCHECKPOINT_HEADERS}
)

target_compile_features(CheckpointPlugin PRIVATE cxx_std_17)

target_include_directories(CheckpointPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/fmt/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/IMGUI"
    "${_bakkesmod_include_dir}"
)

target_link_libraries(CheckpointPlugin PRIVATE
    "${BAKKESMOD_PLUGINSDK}"
)

# Match the original Visual Studio project's Windows character set and
# console-related definitions.
target_compile_definitions(CheckpointPlugin PRIVATE
    UNICODE
    _UNICODE
    _CONSOLE
    # fmt v7 bundled by the original plugin uses Microsoft's deprecated
    # checked_array_iterator API. Newer MSVC versions emit C4996 for it.
    _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING
)

# Keep warning/conformance behavior close to the upstream .vcxproj.
target_compile_options(CheckpointPlugin PRIVATE
    /W3
    /sdl
    /permissive-
    /MP
)

# Match the original Release linker optimizations.
target_link_options(CheckpointPlugin PRIVATE
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
)

# The upstream project enables Whole Program Optimization in Release.
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_error)
if(_ipo_supported)
    set_property(TARGET CheckpointPlugin
        PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )
else()
    message(STATUS "IPO/LTO unavailable: ${_ipo_error}")
endif()

# Use CMake's PCH support rather than relying on Visual Studio project-specific
# /Yc and /Yu settings.
target_precompile_headers(CheckpointPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/pch.h"
)

# Put the DLL in a predictable build/plugins directory for every generator
# and every configuration.
set(_plugin_output_dir "${CMAKE_BINARY_DIR}/plugins")

set_target_properties(CheckpointPlugin PROPERTIES
    OUTPUT_NAME "CheckpointPlugin"
    RUNTIME_OUTPUT_DIRECTORY "${_plugin_output_dir}"
    LIBRARY_OUTPUT_DIRECTORY "${_plugin_output_dir}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

foreach(_config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_config}" _config_upper)

    set_target_properties(CheckpointPlugin PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${_config_upper}" "${_plugin_output_dir}"
        "LIBRARY_OUTPUT_DIRECTORY_${_config_upper}" "${_plugin_output_dir}"
        "ARCHIVE_OUTPUT_DIRECTORY_${_config_upper}" "${CMAKE_BINARY_DIR}/lib"
    )
endforeach()

# BakkesMod requires its patch utility to process plugin DLLs after linking.
add_custom_command(
    TARGET CheckpointPlugin
    POST_BUILD
    COMMAND "${BAKKESMOD_PATCHER}" "$<TARGET_FILE:CheckpointPlugin>"
    COMMENT "Patching CheckpointPlugin.dll for BakkesMod"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Optional developer conveniences
# ---------------------------------------------------------------------------

# The original .vcxproj increments VERSION_BUILD for Release builds using
# update_version.ps1. Doing that on every CMake build would mutate a tracked
# source file, so it is opt-in here.
option(
    FREEPLAYCHECKPOINT_INCREMENT_BUILD_VERSION
    "Run update_version.ps1 before building (modifies version.h)"
    OFF
)

if(FREEPLAYCHECKPOINT_INCREMENT_BUILD_VERSION)
    find_program(
        POWERSHELL_EXECUTABLE
        NAMES pwsh.exe powershell.exe
        REQUIRED
    )

    add_custom_target(CheckpointPluginVersion
        COMMAND "${POWERSHELL_EXECUTABLE}"
            -ExecutionPolicy Bypass
            -NoProfile
            -NonInteractive
            -File "${CMAKE_CURRENT_SOURCE_DIR}/update_version.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/version.h"
        COMMENT "Incrementing VERSION_BUILD in version.h"
        VERBATIM
    )

    add_dependencies(CheckpointPlugin CheckpointPluginVersion)
endif()

# Optional automatic deployment for local development.
# Disabled by default because the DLL may be locked while Rocket League is
# running and because a build should not normally modify an installed plugin.
option(
    FREEPLAYCHECKPOINT_DEPLOY_TO_BAKKESMOD
    "Copy the built plugin into the local BakkesMod plugins directory"
    OFF
)

if(FREEPLAYCHECKPOINT_DEPLOY_TO_BAKKESMOD)
    if(NOT BAKKESMOD_PATH)
        message(FATAL_ERROR
            "FREEPLAYCHECKPOINT_DEPLOY_TO_BAKKESMOD requires BAKKESMOD_PATH."
        )
    endif()

    set(_bakkesmod_plugins_dir "${BAKKESMOD_PATH}/plugins")

    add_custom_command(
        TARGET CheckpointPlugin
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${_bakkesmod_plugins_dir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:CheckpointPlugin>"
            "${_bakkesmod_plugins_dir}/CheckpointPlugin.dll"
        COMMENT "Deploying CheckpointPlugin.dll to BakkesMod"
        VERBATIM
    )
endif()

message(STATUS
    "CheckpointPlugin output: ${_plugin_output_dir}/CheckpointPlugin.dll"
)
