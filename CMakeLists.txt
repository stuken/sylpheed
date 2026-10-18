# sylpheed - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(sylpheed LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CMakeDependentOption)
# OFF keeps the SDK's xenos GPU plugin (Xenia-style emulation). ON replaces the
# guest D3D library with the native plume renderer ported from rr6 and the
# recompiled shader cache. Hooks bind at link time, so this is a build switch,
# not a cvar. Windows builds D3D12 and Vulkan exes, Linux the Vulkan one.
cmake_dependent_option(SYLPHEED_NATIVE_GPU
    "Native plume renderer with recompiled shaders (OFF = SDK xenos plugin)"
    OFF "WIN32 OR LINUX" OFF)

include(generated/rexglue.cmake)

# Embedded window icon: the title icon from assets/default.xex (already a
# codegen input), so no extra asset is checked in or fetched. sylpheed_icon
# decrypts and LZX-decompresses the XEX with the SDK's lzx_decompress and
# writes src/icon.generated.h and src/icon.generated.ico, gitignored build
# artifacts regenerated when the XEX or the tool changes.
add_executable(sylpheed_icon tools/extract_icon.cpp)
target_link_libraries(sylpheed_icon PRIVATE rex::runtime)
if(WIN32)
    # It runs during the build, before anything has copied the runtime DLL
    # next to the executables.
    add_custom_command(TARGET sylpheed_icon POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:sylpheed_icon> $<TARGET_FILE_DIR:sylpheed_icon>
        COMMAND_EXPAND_LISTS)
endif()
set(SYLPHEED_ICON_HEADER ${CMAKE_SOURCE_DIR}/src/icon.generated.h)
set(SYLPHEED_ICON_ICO ${CMAKE_SOURCE_DIR}/src/icon.generated.ico)
add_custom_command(
    OUTPUT ${SYLPHEED_ICON_HEADER} ${SYLPHEED_ICON_ICO}
    COMMAND sylpheed_icon ${CMAKE_SOURCE_DIR}/assets/default.xex
            ${SYLPHEED_ICON_HEADER} ${SYLPHEED_ICON_ICO}
    DEPENDS sylpheed_icon ${CMAKE_SOURCE_DIR}/assets/default.xex
    COMMENT "Extracting the title icon from assets/default.xex"
    VERBATIM)

# On Windows the same icon is also the executables' own resource (Explorer,
# the taskbar before the window opens, Alt-Tab), apart from the in-window icon
# set at runtime (sylpheed_app.h). The .rc takes the .ico's absolute path, as
# it is not next to it in an out-of-source build.
set(SYLPHEED_ICON_RC "")
if(WIN32)
    set(SYLPHEED_ICON_ICO_PATH ${SYLPHEED_ICON_ICO})
    set(SYLPHEED_ICON_RC ${CMAKE_SOURCE_DIR}/src/sylpheed.generated.rc)
    configure_file(${CMAKE_SOURCE_DIR}/src/sylpheed.rc.in ${SYLPHEED_ICON_RC} @ONLY)
    # configure_file only stamps the path in: the .ico must exist before the
    # resource compiler reads it.
    set_source_files_properties(${SYLPHEED_ICON_RC} PROPERTIES OBJECT_DEPENDS
                                ${SYLPHEED_ICON_ICO})
endif()

# Sources
set(SYLPHEED_SOURCES
    src/main.cpp
    ${SYLPHEED_ICON_HEADER}
)

if(WIN32)
    add_executable(sylpheed WIN32 ${SYLPHEED_SOURCES} ${SYLPHEED_ICON_RC})
else()
    add_executable(sylpheed ${SYLPHEED_SOURCES})
endif()

# Test tooling both renderers share (sylpheed_input_script).
target_sources(sylpheed PRIVATE src/platform/input_script.cpp)
# Game-logic hooks both renderers share (sylpheed_lod_scale).
target_sources(sylpheed PRIVATE src/game/lod_scale.cpp)
target_include_directories(sylpheed PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(SYLPHEED_NATIVE_GPU)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # Recompiled Xenos shaders: XenosRecomp turns every shader container the
    # game ships in Common.xpr, plus the ones the renderer dumped at run time,
    # into a DXIL/SPIR-V cache compiled into sylpheed_shader_cache.
    set(SYLPHEED_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(SYLPHEED_SHADER_COMMON_H
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/XenosRecomp/shader_common.h")
    cmake_path(GET SYLPHEED_SHADER_COMMON_H PARENT_PATH SYLPHEED_SHADER_COMMON_DIR)

    # Shaders the renderer saved on a cache miss (sylpheed_shader_dump): those
    # next to this build's exe, and any copied into assets/shaders.
    set(SYLPHEED_SHADER_DUMP_DIRS
        "${CMAKE_CURRENT_BINARY_DIR}/user/shaders"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders"
        CACHE STRING "Directories of dumped .vso/.pso shaders to recompile")

    add_subdirectory(thirdparty)
    include(cmake/shader_cache.cmake)

    sylpheed_shader_cache(
        RECOMP_TARGET XenosRecomp
        GAME_ROOT     "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        DUMP_DIRS     ${SYLPHEED_SHADER_DUMP_DIRS}
        SHADER_DIR    "${SYLPHEED_GEN_DIR}/shaders"
        INCLUDE_FILE  "${SYLPHEED_SHADER_COMMON_H}"
        OUTPUT_CPP    "${SYLPHEED_GEN_DIR}/shader_cache.cpp")

    add_library(sylpheed_shader_cache OBJECT "${SYLPHEED_GEN_DIR}/shader_cache.cpp")
    target_include_directories(sylpheed_shader_cache PRIVATE src)

    rexglue_setup_target(sylpheed)
    include(cmake/native_gpu.cmake)
else()
    rexglue_setup_target(sylpheed GPU_PLUGINS xenos)
    # Resolve-target dumps comparable with the native renderer's frame dumps,
    # and the mission-timed RenderDoc capture both builds share.
    target_sources(sylpheed PRIVATE src/platform/xenos_resolve_dump.cpp
                                    src/platform/renderdoc_trigger.cpp)
    target_include_directories(sylpheed PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# The recompiled guest code, optimized even in Debug builds. Its VMX128 vector
# ops go through the SDK's emulation helpers (rex::ppc::simde_mm_sllv_epi16 and
# friends), which unoptimized cost dozens of calls per guest instruction: the
# opening movie's decoder spent 60% of its time in three of them and played at
# a third of its speed. The hooks and renderer in src/ stay unoptimized.
option(SYLPHEED_OPTIMIZE_RECOMP
    "Compile the recompiled guest code with optimizations in Debug builds" ON)
if(SYLPHEED_OPTIMIZE_RECOMP AND TARGET sylpheed_recomp)
    target_compile_options(sylpheed_recomp PRIVATE
        $<$<CONFIG:Debug>:$<IF:$<CXX_COMPILER_FRONTEND_VARIANT:MSVC>,/O2,-O2>>)
endif()
