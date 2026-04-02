# ==============================================================================
#
#  This file is part of the JUCE framework.
#  Copyright (c) Raw Material Software Limited
#
#  JUCE is an open source framework subject to commercial or open source
#  licensing.
#
#  By downloading, installing, or using the JUCE framework, or combining the
#  JUCE framework with any other source code, object code, content or any other
#  copyrightable work, you agree to the terms of the JUCE End User Licence
#  Agreement, and all incorporated terms including the JUCE Privacy Policy and
#  the JUCE Website Terms of Service, as applicable, which will bind you. If you
#  do not agree to the terms of these agreements, we will not license the JUCE
#  framework to you, and you must discontinue the installation or download
#  process and cease use of the JUCE framework.
#
#  JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
#  JUCE Privacy Policy: https://juce.com/juce-privacy-policy
#  JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/
#
#  Or:
#
#  You may also use this code under the terms of the AGPLv3:
#  https://www.gnu.org/licenses/agpl-3.0.en.html
#
#  THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
#  WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
#  MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.
#
# ==============================================================================

# Patched for OrbitLooper Android cross-compilation.
# Change vs. original: adds an early-exit in the else() branch when
# JUCEToolsExport.cmake has been pre-created by the Android CMakeLists.txt.
# This avoids re-invoking cmake (which fails when rc.exe is absent from the
# Gradle process PATH).  The original file is at:
#   JUCE/extras/Build/juceaide/CMakeLists.txt

if(JUCE_BUILD_HELPER_TOOLS)
    # Build the tool for the current system
    juce_add_console_app(juceaide _NO_RESOURCERC)

    target_sources(juceaide PRIVATE Main.cpp)

    target_compile_definitions(juceaide PRIVATE
        JUCE_DISABLE_JUCE_VERSION_PRINTING=1
        JUCE_USE_CURL=0
        JUCE_SILENCE_XCODE_15_LINKER_WARNING=1)

    target_link_libraries(juceaide PRIVATE
        juce::juce_build_tools
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

    set_target_properties(juceaide PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")

    export(TARGETS juceaide
           NAMESPACE juce_tools::
           FILE "${JUCE_BINARY_DIR}/JUCEToolsExport.cmake")
else()
    message(STATUS "Configuring juceaide")

    # ── Android cross-compilation bootstrap ───────────────────────────────────
    # If JUCEToolsExport.cmake was pre-created (pointing at the host juceaide.exe
    # built by the desktop Windows build), skip the inner cmake+build entirely.
    # Without this, JUCE tries to re-invoke cmake to build juceaide for the host,
    # which fails inside Gradle because rc.exe is not in the process PATH.
    if(EXISTS "${JUCE_BINARY_DIR}/tools/JUCEToolsExport.cmake")
        message(STATUS "OrbitLooper: Pre-existing JUCEToolsExport.cmake found — skipping juceaide host build")
        include("${JUCE_BINARY_DIR}/tools/JUCEToolsExport.cmake")
        add_executable(juceaide IMPORTED GLOBAL)
        get_target_property(imported_location juce_tools::juceaide IMPORTED_LOCATION_CUSTOM)
        set_target_properties(juceaide PROPERTIES IMPORTED_LOCATION "${imported_location}")
        add_executable(juce::juceaide ALIAS juceaide)
        return()
    endif()
    # ──────────────────────────────────────────────────────────────────────────

    if(CMAKE_CROSSCOMPILING)
        unset(ENV{ADDR2LINE})
        unset(ENV{AR})
        unset(ENV{ASM})
        unset(ENV{AS})
        unset(ENV{CC})
        unset(ENV{CPP})
        unset(ENV{CXXFILT})
        unset(ENV{CXX})
        unset(ENV{DLLTOOL})
        unset(ENV{DLLWRAP})
        unset(ENV{ELFEDIT})
        unset(ENV{GCC})
        unset(ENV{GCOV_DUMP})
        unset(ENV{GCOV_TOOL})
        unset(ENV{GCOV})
        unset(ENV{GPROF})
        unset(ENV{GXX})
        unset(ENV{LDFLAGS})
        unset(ENV{LD_BFD})
        unset(ENV{LD})
        unset(ENV{LTO_DUMP})
        unset(ENV{NM})
        unset(ENV{OBJCOPY})
        unset(ENV{OBJDUMP})
        unset(ENV{PKG_CONFIG_LIBDIR})
        unset(ENV{PKG_CONFIG})
        unset(ENV{RANLIB})
        unset(ENV{RC})
        unset(ENV{READELF})
        unset(ENV{SIZE})
        unset(ENV{STRINGS})
        unset(ENV{STRIP})
        unset(ENV{WIDL})
        unset(ENV{WINDMC})
        unset(ENV{WINDRES})

        if(DEFINED ENV{PATH_ORIG})
            set(ENV{PATH} "$ENV{PATH_ORIG}")
        endif()
    endif()

    # Generator platform is only supported on specific generators
    if(CMAKE_GENERATOR MATCHES "^Visual Studio.*$")
        set(ENV{CMAKE_GENERATOR_PLATFORM} "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()

    set(PASSTHROUGH_ARGS "")

    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND PASSTHROUGH_ARGS "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()

    if(CMAKE_C_COMPILER_LAUNCHER)
        list(APPEND PASSTHROUGH_ARGS "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
    endif()

    if(CMAKE_CXX_COMPILER_LAUNCHER)
        list(APPEND PASSTHROUGH_ARGS "-DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}")
    endif()

    # Looks like we're bootstrapping, reinvoke CMake
    execute_process(COMMAND "${CMAKE_COMMAND}"
            "."
            "-B${JUCE_BINARY_DIR}/tools"
            "-G${CMAKE_GENERATOR}"
            "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DCMAKE_CONFIGURATION_TYPES=Custom"
            "-DCMAKE_BUILD_TYPE=Custom"
            "-DJUCE_BUILD_HELPER_TOOLS=ON"
            "-DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}"
            ${PASSTHROUGH_ARGS}
        WORKING_DIRECTORY "${JUCE_SOURCE_DIR}"
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_output
        RESULT_VARIABLE result_variable)

    if(result_variable)
        message(FATAL_ERROR "Failed to configure juceaide\n${command_output}")
    endif()

    message(STATUS "Building juceaide")

    execute_process(COMMAND "${CMAKE_COMMAND}"
            --build "${JUCE_BINARY_DIR}/tools"
            --config Custom
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_output
        RESULT_VARIABLE result_variable)

    if(result_variable)
        message(FATAL_ERROR "Failed to build juceaide\n${command_output}")
    endif()

    message(STATUS "Exporting juceaide")

    # This will be generated by the recursive invocation of CMake (above)
    include("${JUCE_BINARY_DIR}/tools/JUCEToolsExport.cmake")

    add_executable(juceaide IMPORTED GLOBAL)
    get_target_property(imported_location juce_tools::juceaide IMPORTED_LOCATION_CUSTOM)
    set_target_properties(juceaide PROPERTIES IMPORTED_LOCATION "${imported_location}")

    add_executable(juce::juceaide ALIAS juceaide)

    set(JUCE_TOOL_INSTALL_DIR "bin/JUCE-${JUCE_VERSION}" CACHE STRING
        "The location, relative to the install prefix, where juceaide will be installed")

    install(PROGRAMS "${imported_location}" DESTINATION "${JUCE_TOOL_INSTALL_DIR}")

    get_filename_component(binary_name "${imported_location}" NAME)
    set(JUCE_JUCEAIDE_NAME "${binary_name}" CACHE INTERNAL "The name of the juceaide program")

    message(STATUS "Testing juceaide")

    execute_process(COMMAND "${imported_location}" version
        RESULT_VARIABLE result_variable
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)

    if(result_variable)
        message(WARNING "Testing juceaide failed (code: ${result_variable}):\noutput: ${output}")
    endif()

    message(STATUS "Finished setting up juceaide")
endif()