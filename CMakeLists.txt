cmake_minimum_required(VERSION 3.22)
project(OrbitLooper VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Detect/normalize target architecture for Windows generators.
set(ORBITLOOPER_WINDOWS_ARM64 FALSE)
if(WIN32)
    string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" ORBITLOOPER_GENERATOR_PLATFORM_LOWER)
    string(TOLOWER "${CMAKE_VS_PLATFORM_NAME}" ORBITLOOPER_VS_PLATFORM_NAME_LOWER)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" ORBITLOOPER_SYSTEM_PROCESSOR_LOWER)

    if(ORBITLOOPER_GENERATOR_PLATFORM_LOWER STREQUAL "arm64"
       OR ORBITLOOPER_VS_PLATFORM_NAME_LOWER STREQUAL "arm64"
       OR ORBITLOOPER_SYSTEM_PROCESSOR_LOWER STREQUAL "arm64")
        set(ORBITLOOPER_WINDOWS_ARM64 TRUE)

        # JUCE's FindWebView2.cmake checks CMAKE_SYSTEM_PROCESSOR against lowercase arm64.
        set(CMAKE_SYSTEM_PROCESSOR arm64)
        set(CMAKE_SYSTEM_PROCESSOR arm64 CACHE STRING "Normalized ARM64 processor for JUCE WebView2 detection" FORCE)
    endif()

    message(STATUS "Orbit Looper: Windows generator platform='${CMAKE_GENERATOR_PLATFORM}', VS platform='${CMAKE_VS_PLATFORM_NAME}', system processor='${CMAKE_SYSTEM_PROCESSOR}', arm64=${ORBITLOOPER_WINDOWS_ARM64}")
endif()

# ============================================
# FETCH JUCE
# ============================================
include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.12
    GIT_SHALLOW    TRUE
)

# Apply patch to JUCE before configuring it (Linux only)
FetchContent_GetProperties(JUCE)
if(NOT juce_POPULATED)
    FetchContent_Populate(JUCE)

    if(UNIX AND NOT APPLE)
        message(STATUS "Apply JUCE Linux WebView fix (overwrite file)...")
        configure_file(
            "${CMAKE_SOURCE_DIR}/Source/patches/juce_WebBrowserComponent_linux.patched.cpp"
            "${juce_SOURCE_DIR}/modules/juce_gui_extra/native/juce_WebBrowserComponent_linux.cpp"
            COPYONLY
        )
        message(STATUS "Replaced juce_WebBrowserComponent_linux.cpp with patched version.")
    endif()

    if(WIN32)
        message(STATUS "Patching JUCE juceaide/CMakeLists.txt to bypass headless execution test...")
        set(JUCEAIDE_CMAKE "${juce_SOURCE_DIR}/extras/Build/juceaide/CMakeLists.txt")
        file(READ "${JUCEAIDE_CMAKE}" _juceaide_cmake)
        string(REPLACE "message(FATAL_ERROR \"Testing juceaide failed:\\noutput: \${output}\")" "message(WARNING \"Testing juceaide failed (code: \${result_variable}):\\noutput: \${output}\")" _juceaide_cmake "${_juceaide_cmake}")
        file(WRITE "${JUCEAIDE_CMAKE}" "${_juceaide_cmake}")

        # Patch JUCEUtils.cmake: skip winicon generation if a pre-built icon.ico
        # exists in assets/. This allows headless Windows CI (Server Core) to work
        # without running juceaide, which depends on GUI DLLs (dcomp, d2d1, d3d11).
        if(EXISTS "${CMAKE_SOURCE_DIR}/assets/icon.ico")
            message(STATUS "Patching JUCE JUCEUtils.cmake to use pre-built icon.ico...")
            set(JUCE_UTILS_CMAKE "${juce_SOURCE_DIR}/extras/Build/CMake/JUCEUtils.cmake")
            file(READ "${JUCE_UTILS_CMAKE}" _juce_utils)
            string(REPLACE
                "set(generated_icon \"\${juce_library_code}/icon.ico\")\n        _juce_execute_juceaide(winicon \"\${generated_icon}\" \${icon_args})"
                "set(generated_icon \"\${juce_library_code}/icon.ico\")\n        if(EXISTS \"${CMAKE_SOURCE_DIR}/assets/icon.ico\")\n            file(COPY \"${CMAKE_SOURCE_DIR}/assets/icon.ico\" DESTINATION \"\${juce_library_code}\")\n            message(STATUS \"Using pre-built icon.ico (skipping juceaide winicon)\")\n        else()\n            _juce_execute_juceaide(winicon \"\${generated_icon}\" \${icon_args})\n        endif()"
                _juce_utils "${_juce_utils}")
            file(WRITE "${JUCE_UTILS_CMAKE}" "${_juce_utils}")
        endif()
    endif()

    add_subdirectory(${juce_SOURCE_DIR} ${juce_BINARY_DIR})
endif()

if(WIN32 AND ORBITLOOPER_WINDOWS_ARM64)
    # JUCE may still resolve x64 WebView2 static lib on cross-compile hosts.
    # Force the juce_webview2 interface target to link ARM64 static loader.
    set(_wv2_package_root "$ENV{USERPROFILE}/AppData/Local/PackageManagement/NuGet/Packages")
    file(GLOB _wv2_package_dirs "${_wv2_package_root}/Microsoft.Web.WebView2*")

    set(ORBITLOOPER_ARM64_WEBVIEW2_LIB "")
    foreach(_pkg_dir IN LISTS _wv2_package_dirs)
        if(EXISTS "${_pkg_dir}/build/native/arm64/WebView2LoaderStatic.lib")
            set(ORBITLOOPER_ARM64_WEBVIEW2_LIB "${_pkg_dir}/build/native/arm64/WebView2LoaderStatic.lib")
        endif()
    endforeach()

    if(ORBITLOOPER_ARM64_WEBVIEW2_LIB)
        if(TARGET juce_webview2)
            set_property(TARGET juce_webview2 PROPERTY INTERFACE_LINK_LIBRARIES "${ORBITLOOPER_ARM64_WEBVIEW2_LIB}")
            message(STATUS "Orbit Looper: Forced juce_webview2 to ARM64 static lib ${ORBITLOOPER_ARM64_WEBVIEW2_LIB}")
        else()
            message(WARNING "Orbit Looper: juce_webview2 target not found for ARM64 WebView2 override")
        endif()
    else()
        message(WARNING "Orbit Looper: ARM64 WebView2LoaderStatic.lib not found in ${_wv2_package_root}")
    endif()
endif()

# ============================================
# PLATFORM DETECTION & FORMAT CONFIGURATION
# ============================================
if(WIN32)
    set(PLUGIN_FORMATS VST3 Standalone)
    set(NEEDS_WEBVIEW2 TRUE)
    set(NEEDS_WEB_BROWSER FALSE)
    set(WEBVIEW_BACKEND "WebView2")
    set(JUCE_USE_WIN_WEBVIEW2 ON CACHE BOOL "" FORCE)
    add_definitions(-DJUCE_USE_WIN_WEBVIEW2=1)
elseif(APPLE)
    set(PLUGIN_FORMATS VST3 AU Standalone)
    set(NEEDS_WEBVIEW2 FALSE)
    set(NEEDS_WEB_BROWSER FALSE)
    set(WEBVIEW_BACKEND "WKWebView")
    set(AU_MAIN_TYPE kAudioUnitType_Effect)
elseif(UNIX)
    set(PLUGIN_FORMATS VST3 LV2 Standalone)
    set(NEEDS_WEBVIEW2 FALSE)
    set(NEEDS_WEB_BROWSER TRUE)
    set(WEBVIEW_BACKEND "WebKitGTK")
    set(LV2_URI "https://github.com/dev-dhg/orbit-looper")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# = [options] =
option(ORBITLOOPER_LINUX_WAYLAND "Use Wayland backend for WebView on Linux" OFF)

message(STATUS "Orbit Looper: Platform formats: ${PLUGIN_FORMATS}")
message(STATUS "Orbit Looper: WebView backend: ${WEBVIEW_BACKEND}")
if(UNIX AND NOT APPLE)
    message(STATUS "Orbit Looper: Linux Wayland support: ${ORBITLOOPER_LINUX_WAYLAND}")
endif()

# ============================================
# BINARY DATA (Embedded Web UI)
# ============================================
juce_add_binary_data(OrbitLooper_WebUI
    SOURCES
        Source/ui/public/index.html
        Source/ui/public/style.css
        Source/ui/public/juce-bridge.js
        Source/ui/public/state.js
        Source/ui/public/ui-controls.js
        Source/ui/public/transport.js
        Source/ui/public/metronome.js
        Source/ui/public/keymapping.js
        Source/ui/public/main.js
)

# ============================================
# PLUGIN DEFINITION
# ============================================
juce_add_plugin(OrbitLooper
    COMPANY_NAME "dev-dhg"
    PLUGIN_MANUFACTURER_CODE Ddhg
    PLUGIN_CODE OrLp
    FORMATS ${PLUGIN_FORMATS}
    PRODUCT_NAME "Orbit Looper"
    NEEDS_WEBVIEW2 ${NEEDS_WEBVIEW2}
    NEEDS_WEB_BROWSER ${NEEDS_WEB_BROWSER}
    VST3_CATEGORIES Fx Delay
    AU_MAIN_TYPE ${AU_MAIN_TYPE}
    LV2URI ${LV2_URI}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    ICON_BIG assets/icon.png
)

# ============================================
# SOURCE FILES
# ============================================
target_sources(OrbitLooper
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/BluetoothClassicMidi.cpp
        Source/BluetoothClassicMidi.h
        Source/ui/OrbitLooperWebBrowser.cpp
        Source/ui/OrbitLooperWebBrowser.h
)

target_include_directories(OrbitLooper
    PRIVATE
        Source
)

# ============================================
# LINK LIBRARIES
# ============================================
target_link_libraries(OrbitLooper
    PRIVATE
        OrbitLooper_WebUI
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ============================================
# COMPILE DEFINITIONS
# ============================================
target_compile_definitions(OrbitLooper
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_VST3_CAN_REPLACE_VST2=0
        _USE_MATH_DEFINES=1
)

if(WIN32)
    target_compile_definitions(OrbitLooper PUBLIC JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)

    # ASIO support — JUCE 8.0.11+ bundles ASIO headers, no external SDK needed
    target_compile_definitions(OrbitLooper PUBLIC JUCE_ASIO=1)
endif()

if(APPLE)
    target_compile_definitions(OrbitLooper
        PUBLIC
            JUCE_USE_CURL=0
    )
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(OrbitLooper
        PUBLIC
            JUCE_USE_CURL=0
            JUCE_JACK=1
            JUCE_MODAL_LOOPS_PERMITTED=1
    )

    if(ORBITLOOPER_LINUX_WAYLAND)
        target_compile_definitions(OrbitLooper PUBLIC ORBITLOOPER_WAYLAND=1)
    endif()


endif()
