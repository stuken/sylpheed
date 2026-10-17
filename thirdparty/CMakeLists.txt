# XenosRecomp runs on the build host and turns SYLPHEED's Xenos shaders into the
# embedded DXIL/SPIR-V cache. RR6_RECOMP (named for the branch's first title)
# selects the container revision 0x0E handling Sylpheed's XDK 2.0.3215 shaders
# also use, and keeps Blue Dragon's shader rewrites out.
# REBLUE_RECOMP selects the shader ABI shared with the reblue-derived renderer.
add_subdirectory(XenosRecomp EXCLUDE_FROM_ALL)
target_compile_definitions(XenosRecomp PRIVATE REBLUE_RECOMP RR6_RECOMP)
