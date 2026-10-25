cmake_minimum_required(VERSION 3.20)
# Windows CLI -D tool paths may contain backslashes; normalize BEFORE project(),
# whose Windows-Clang module can enable RC while detecting the CXX compiler.
foreach(tool CMAKE_CXX_COMPILER CMAKE_RC_COMPILER CMAKE_OBJDUMP CMAKE_MAKE_PROGRAM)
    if(DEFINED ${tool})
        file(TO_CMAKE_PATH "${${tool}}" normalized_tool)
        set(${tool} "${normalized_tool}" CACHE FILEPATH "Explicit portable tool path" FORCE)
    endif()
endforeach()
if(CMAKE_GENERATOR STREQUAL "MinGW Makefiles")
    foreach(path "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
            "${CMAKE_CXX_COMPILER}" "${CMAKE_RC_COMPILER}" "${CMAKE_MAKE_PROGRAM}" "${CMAKE_COMMAND}")
        if(path MATCHES "[^ -~]")
            message(FATAL_ERROR "MinGW Makefiles uses an ANSI Windows make backend with non-ASCII paths. Use -G Ninja with a trusted ninja executable, or use build.ps1 (native Unicode paths; no CMake required).")
        endif()
    endforeach()
endif()
# Ninja must agree with Clang's UTF-8 depfiles when Windows paths are non-ASCII.
# An ANSI Ninja can appear to pass a clean build yet fail on the next increment.
if(CMAKE_HOST_WIN32 AND CMAKE_GENERATOR MATCHES "^Ninja" AND CMAKE_MAKE_PROGRAM)
    set(lol_non_ascii_path OFF)
    foreach(path "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CXX_COMPILER}")
        if(path MATCHES "[^ -~]")
            set(lol_non_ascii_path ON)
        endif()
    endforeach()
    if(lol_non_ascii_path)
        execute_process(COMMAND "${CMAKE_MAKE_PROGRAM}" -t wincodepage
            RESULT_VARIABLE lol_ninja_status OUTPUT_VARIABLE lol_ninja_codepage ERROR_QUIET)
        if(lol_ninja_status EQUAL 0 AND lol_ninja_codepage MATCHES "ANSI")
            message(FATAL_ERROR "ANSI Ninja cannot safely read Clang UTF-8 dependency paths here. Select a trusted UTF-8-manifest Ninja with -DCMAKE_MAKE_PROGRAM, use build.ps1, or use entirely ASCII paths; no system code-page change is needed.")
        endif()
    endif()
endif()
project(LOLBackupV2 VERSION 3.3.17 LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "LOLBackupV2 requires a Windows target (native Win32 or a MinGW cross toolchain)")
endif()
set(LOL_ARCHITECTURE "x86" CACHE STRING "Target architecture: x86 (primary) or x64 (development)")
set_property(CACHE LOL_ARCHITECTURE PROPERTY STRINGS x86 x64)
if(LOL_ARCHITECTURE STREQUAL "x86")
    set(LOL_POINTER_SIZE 4)
    set(LOL_RESOURCE_TARGET pe-i386)
elseif(LOL_ARCHITECTURE STREQUAL "x64")
    set(LOL_POINTER_SIZE 8)
    set(LOL_RESOURCE_TARGET pe-x86-64)
else()
    message(FATAL_ERROR "LOL_ARCHITECTURE must be x86 or x64")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL LOL_POINTER_SIZE)
    message(FATAL_ERROR "Compiler does not match LOL_ARCHITECTURE=${LOL_ARCHITECTURE}; select the matching i686/x86_64 toolchain in a separate build directory")
endif()
set(LOL_EXECUTABLE_NAME "LOLBackup" CACHE STRING "Single-EXE product basename")
if(NOT LOL_EXECUTABLE_NAME MATCHES "^(LOLBackup|LOLBackupV3)$")
    message(FATAL_ERROR "LOL_EXECUTABLE_NAME must be LOLBackup or LOLBackupV3")
endif()
enable_language(RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# No global Unicode entry-point or GUI subsystem flags: tests use ordinary main().
add_library(lolbackup_core STATIC
    src/core/service.cpp
    src/core/sync_engine.cpp
    src/core/file_ops.cpp
    src/core/path_policy.cpp
    src/core/root_store.cpp
    src/core/logger.cpp
    src/core/config.cpp
    src/core/monitor.cpp
    src/core/game_detect.cpp
    src/core/tool_manager.cpp)
target_include_directories(lolbackup_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json")
target_compile_definitions(lolbackup_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000000)
target_link_libraries(lolbackup_core PUBLIC user32 advapi32 shell32 ole32 uuid shlwapi version cabinet)
if(MSVC)
    target_compile_options(lolbackup_core PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/permissive->")
else()
    target_compile_options(lolbackup_core PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-finput-charset=UTF-8;-fexec-charset=UTF-8>")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(LOL_SIZE_OPT -O2)
    else()
        set(LOL_SIZE_OPT -O2)
    endif()
    target_compile_options(lolbackup_core PUBLIC "$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:${LOL_SIZE_OPT};-g0;-ffunction-sections;-fdata-sections>")
    target_link_options(lolbackup_core INTERFACE -static -Wl,--dynamicbase -Wl,--nxcompat
        -Wl,--major-os-version,6,--minor-os-version,1,--major-subsystem-version,6,--minor-subsystem-version,1
        "$<$<CONFIG:Release>:-s;-Wl,--gc-sections>")
    if(LOL_ARCHITECTURE STREQUAL "x64")
        target_link_options(lolbackup_core INTERFACE -Wl,--high-entropy-va)
    else()
        target_link_options(lolbackup_core INTERFACE -Wl,--large-address-aware)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(lolbackup_core INTERFACE -static-libgcc -static-libstdc++)
    endif()
endif()

# RC is compiled to an object and linked into this target, never post-link relinking.
# All RC resources, including embedded UI images/fonts/licenses, stay in this PE.
add_executable(LOLBackupV2 WIN32 src/app/main.cpp resources/app.rc)
set_target_properties(LOLBackupV2 PROPERTIES OUTPUT_NAME "${LOL_EXECUTABLE_NAME}")
target_include_directories(LOLBackupV2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_link_libraries(LOLBackupV2 PRIVATE lolbackup_core user32 gdi32 gdiplus comctl32 dwmapi dbghelp cabinet)
if(MINGW)
    set_source_files_properties(resources/app.rc PROPERTIES COMPILE_OPTIONS "--target=${LOL_RESOURCE_TARGET}")
    target_link_options(LOLBackupV2 PRIVATE -municode)
endif()

if(NOT CMAKE_OBJDUMP)
    find_program(CMAKE_OBJDUMP NAMES llvm-objdump i686-w64-mingw32-objdump x86_64-w64-mingw32-objdump objdump REQUIRED)
endif()
function(audit_native_target target)
    get_target_property(is_gui ${target} WIN32_EXECUTABLE)
    if(is_gui)
        set(subsystem 2)
    else()
        set(subsystem 3)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DBINARY=$<TARGET_FILE:${target}>"
            "-DOBJDUMP=${CMAKE_OBJDUMP}" "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DARCHITECTURE=${LOL_ARCHITECTURE}" "-DSUBSYSTEM=${subsystem}" "-DCONFIGURATION=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyBinary.cmake"
        VERBATIM)
endfunction()
audit_native_target(LOLBackupV2)

include(CTest)
if(BUILD_TESTING)
    set(LOL_TESTS test_sync test_sync_semantics test_core_bugs test_config_concurrency
        test_bridge_contract test_experience test_stress test_winver test_service_safety
        test_file_safety test_aux_safety test_tool_manager test_ui_geometry)
    foreach(t IN LISTS LOL_TESTS)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${t}.cpp")
            add_executable(${t} "tests/${t}.cpp")
            target_link_libraries(${t} PRIVATE lolbackup_core)
            if(MSVC)
                target_compile_options(${t} PRIVATE /UNDEBUG)
            else()
                target_compile_options(${t} PRIVATE -UNDEBUG)
            endif()
            audit_native_target(${t})
            add_test(NAME ${t} COMMAND ${t})
            set_tests_properties(${t} PROPERTIES TIMEOUT 120)
        else()
            message(FATAL_ERROR "Required regression test is missing: tests/${t}.cpp")
        endif()
    endforeach()
endif()
