# The native renderer (SYLPHEED_NATIVE_GPU), ported from rr6: reblue's plume-based Xenos translation
# layer, fed by the XenosRecomp shader cache, in place of the SDK's emulated
# GPU. The guest's D3D library is replaced by REX_HOOKs in src/gpu/hooks.
#
# Expects the sylpheed target (already through rexglue_setup_target), SYLPHEED_SOURCES,
# SYLPHEED_ICON_RC, SYLPHEED_GEN_DIR, SYLPHEED_SHADER_COMMON_H and the sylpheed_shader_cache library
# to exist already.
enable_language(C)

cmake_dependent_option(SYLPHEED_D3D12
    "Build sylpheed with the D3D12 backend (OFF selects Vulkan)" ON "WIN32" OFF)
cmake_dependent_option(SYLPHEED_VULKAN_EXE
    "Build sylpheed_vk, the Vulkan backend, alongside the D3D12 sylpheed" ON
    "WIN32;SYLPHEED_D3D12" OFF)

set(SYLPHEED_D3D12_TARGETS "")
set(SYLPHEED_VULKAN_TARGETS "")
if(SYLPHEED_D3D12)
    list(APPEND SYLPHEED_D3D12_TARGETS sylpheed)
else()
    list(APPEND SYLPHEED_VULKAN_TARGETS sylpheed)
endif()

if(SYLPHEED_VULKAN_EXE)
    add_executable(sylpheed_vk WIN32 ${SYLPHEED_SOURCES} ${SYLPHEED_ICON_RC})
    target_sources(sylpheed_vk PRIVATE src/platform/input_script.cpp)
    target_include_directories(sylpheed_vk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    # The recompiled guest code never reads the backend, so both exes link the
    # sylpheed_recomp objects rexglue_setup_target made for sylpheed instead of compiling
    # them a second time. The macro only adds them while this list is set.
    set(sylpheed_recomp_sources "${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}")
    set(REXGLUE_ENTRYPOINT_GENERATED_SOURCES "")
    rexglue_setup_target(sylpheed_vk)
    set(REXGLUE_ENTRYPOINT_GENERATED_SOURCES "${sylpheed_recomp_sources}")
    if(TARGET sylpheed_recomp)
        target_link_libraries(sylpheed_vk PRIVATE sylpheed_recomp)
    endif()
    # The SDK stages the same runtime DLLs next to every exe it configures, so
    # serialize the links to keep those POST_BUILD copies off each other.
    add_dependencies(sylpheed_vk sylpheed)
    list(APPEND SYLPHEED_VULKAN_TARGETS sylpheed_vk)
endif()
set(SYLPHEED_NATIVE_TARGETS ${SYLPHEED_D3D12_TARGETS} ${SYLPHEED_VULKAN_TARGETS})

# Linux picks X11 or Wayland at runtime, so plume builds its surface through
# the SDL3 window the SDK already owns (rexglue.cmake has imported SDL3 by now).
if(LINUX)
    set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(thirdparty/plume EXCLUDE_FROM_ALL)
include(cmake/shaders.cmake)

file(GLOB_RECURSE sylpheed_native_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.cpp")
# Built from the top-level CMakeLists: input_script for both renderers,
# xenos_resolve_dump for the plugin build only.
list(REMOVE_ITEM sylpheed_native_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/input_script.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/xenos_resolve_dump.cpp")
# The runtime DXC shader linker has no Vulkan counterpart: SPIR-V takes the
# spec constants as Vulkan specialization constants instead.
set(sylpheed_d3d12_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/dxc_link.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/shader_linker.cpp")
list(REMOVE_ITEM sylpheed_native_sources ${sylpheed_d3d12_sources})
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz/miniz.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/zstddeclib.c"
    PROPERTIES COMPILE_OPTIONS "-w")

foreach(shader IN ITEMS copy_vs imgui_vs)
    sylpheed_host_shader(${shader} vs_6_0)
endforeach()
foreach(shader IN ITEMS
        copy_color_ps copy_depth_ps fxaa_ps gamma_correction_ps
        pfx_occlusion_count_ps imgui_ps)
    sylpheed_host_shader(${shader} ps_6_0)
endforeach()

foreach(target IN LISTS SYLPHEED_NATIVE_TARGETS)
    target_sources(${target} PRIVATE
        ${sylpheed_native_sources}
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz/miniz.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/zstddeclib.c")
    # The recompiler's defines (thirdparty/CMakeLists.txt), so host code reads
    # the same shader_common.h tables (vertex input locations) as the shaders.
    target_compile_definitions(${target} PRIVATE
        SYLPHEED_NATIVE_GPU REBLUE_RECOMP RR6_RECOMP)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${SYLPHEED_GEN_DIR}"
        "${SYLPHEED_SHADER_COMMON_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd")
    target_link_libraries(${target} PRIVATE
        plume
        sylpheed_shader_cache
        rex::xxhash
        rex::o1heap)
endforeach()

if(SYLPHEED_D3D12_TARGETS)
    # sylpheed_prelink DXC-links every specConstantsMask subset of the shader cache
    # at build time, so the runtime never pays a DXC link for a shipped shader.
    add_executable(sylpheed_prelink
        tools/prelink_shader_cache.cpp
        src/gpu/shaders/dxc_link.cpp
        "${SYLPHEED_GEN_DIR}/shader_cache.cpp"
        thirdparty/miniz/miniz.cpp
        thirdparty/zstd/zstddeclib.c)
    target_include_directories(sylpheed_prelink PRIVATE
        src thirdparty/miniz thirdparty/zstd)
    target_link_libraries(sylpheed_prelink PRIVATE Microsoft::DirectXShaderCompiler)
    sylpheed_stage_dxc(sylpheed_prelink)

    add_custom_command(
        OUTPUT "${SYLPHEED_GEN_DIR}/linked_shader_cache.cpp"
        COMMAND sylpheed_prelink "${SYLPHEED_GEN_DIR}/linked_shader_cache.cpp"
        DEPENDS sylpheed_prelink
        COMMENT "Pre-linking spec-constant shader variants"
        VERBATIM)
endif()

foreach(target IN LISTS SYLPHEED_D3D12_TARGETS)
    target_sources(${target} PRIVATE
        ${sylpheed_d3d12_sources}
        "${SYLPHEED_GEN_DIR}/linked_shader_cache.cpp")
    target_compile_definitions(${target} PRIVATE SYLPHEED_D3D12)
    target_link_libraries(${target} PRIVATE
        d3d12 dxgi
        Microsoft::DirectXShaderCompiler)
    sylpheed_stage_dxc(${target})
endforeach()

# The SPIR-V cache stores each module smol-v encoded.
set(sylpheed_smolv_dir "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/source")
set_source_files_properties("${sylpheed_smolv_dir}/smolv.cpp" PROPERTIES COMPILE_OPTIONS "-w")
foreach(target IN LISTS SYLPHEED_VULKAN_TARGETS)
    target_sources(${target} PRIVATE "${sylpheed_smolv_dir}/smolv.cpp")
    target_include_directories(${target} PRIVATE "${sylpheed_smolv_dir}")
endforeach()

# On Windows plume builds VK_KHR_win32_surface straight off the HWND and
# reaches vulkan-1.dll through volk, so the Vulkan backend links nothing extra.
if(NOT WIN32)
    foreach(target IN LISTS SYLPHEED_VULKAN_TARGETS)
        find_package(Vulkan REQUIRED)
        target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
    endforeach()
endif()
