# sylpheed_shader_cache() gathers the game's shader containers at build time,
# runs the Xenos shader recompiler over them into OUTPUT_CPP, and defines
# sylpheed_shader_cache_gen. It also defines sylpheed_shader_hlsl_dump for
# inspecting the intermediate HLSL.
#
# scripts/extract_shaders.py copies the USER resources of
# GAME_ROOT/hidden/resource3d/Common.xpr into SHADER_DIR, plus every shader
# the native renderer dumped on a cache miss into any of DUMP_DIRS (the
# shaders default.xex compiles at run time or keeps compressed). SHADER_DIR
# lives in the build tree, keeping game data out of the source tree. A new
# dump is picked up at the next configure.
function(sylpheed_shader_cache)
    cmake_parse_arguments(ARG "" "RECOMP_TARGET;GAME_ROOT;SHADER_DIR;INCLUDE_FILE;OUTPUT_CPP" "DUMP_DIRS" ${ARGN})
    foreach(arg RECOMP_TARGET GAME_ROOT SHADER_DIR INCLUDE_FILE OUTPUT_CPP)
        if(NOT ARG_${arg})
            message(FATAL_ERROR "sylpheed_shader_cache: missing ${arg}")
        endif()
    endforeach()

    set(common_xpr "${ARG_GAME_ROOT}/hidden/resource3d/Common.xpr")
    set(dump_files "")
    foreach(dir IN LISTS ARG_DUMP_DIRS)
        file(GLOB found CONFIGURE_DEPENDS "${dir}/*.vso" "${dir}/*.pso")
        list(APPEND dump_files ${found})
    endforeach()

    set(extract_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_shaders.py")
    set(shader_index "${ARG_SHADER_DIR}/shaders.csv")
    add_custom_command(
        OUTPUT "${shader_index}"
        COMMAND ${Python3_EXECUTABLE} "${extract_script}" "${ARG_GAME_ROOT}"
                "${ARG_SHADER_DIR}" ${ARG_DUMP_DIRS}
        DEPENDS "${extract_script}" "${common_xpr}" ${dump_files}
        COMMENT "Gathering Xenos shaders from Common.xpr and run-time dumps"
        VERBATIM)

    cmake_path(GET ARG_OUTPUT_CPP PARENT_PATH output_dir)
    file(MAKE_DIRECTORY "${output_dir}")

    add_custom_command(
        OUTPUT "${ARG_OUTPUT_CPP}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_SHADER_DIR}" "${ARG_OUTPUT_CPP}" "${ARG_INCLUDE_FILE}"
        DEPENDS ${ARG_RECOMP_TARGET} "${ARG_INCLUDE_FILE}" "${shader_index}"
        COMMENT "Recompiling Xenos shaders from ${ARG_SHADER_DIR}"
        USES_TERMINAL VERBATIM)
    add_custom_target(sylpheed_shader_cache_gen DEPENDS "${ARG_OUTPUT_CPP}")

    set(SYLPHEED_HLSL_DUMP_DIR "${CMAKE_BINARY_DIR}/hlsl_dump" CACHE PATH
        "Directory sylpheed_shader_hlsl_dump writes recompiled HLSL into")
    add_custom_target(sylpheed_shader_hlsl_dump
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SYLPHEED_HLSL_DUMP_DIR}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_SHADER_DIR}" "${output_dir}/shader_cache.hlsldump.cpp"
                "${ARG_INCLUDE_FILE}" "${SYLPHEED_HLSL_DUMP_DIR}"
        DEPENDS "${shader_index}"
        COMMENT "Dumping recompiled HLSL to ${SYLPHEED_HLSL_DUMP_DIR}"
        USES_TERMINAL VERBATIM)
    add_dependencies(sylpheed_shader_hlsl_dump ${ARG_RECOMP_TARGET})
endfunction()
