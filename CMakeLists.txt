# sylpheed - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(sylpheed LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(SYLPHEED_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(sylpheed WIN32 ${SYLPHEED_SOURCES})
else()
    add_executable(sylpheed ${SYLPHEED_SOURCES})
endif()

rexglue_setup_target(sylpheed GPU_PLUGINS xenos)
