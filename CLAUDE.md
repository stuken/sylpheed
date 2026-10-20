# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A ReXGlue static recompilation of the Xbox 360 game Project Sylpheed. The rexglue SDK (pinned as the `thirdparty/rexglue-sdk` submodule, stuken's fork; found through `rexglue_DIR`/`CMAKE_PREFIX_PATH` pointing at an install, or a source tree via `-DREXSDK_DIR=`) recompiles `assets/default.xex` into C++ under `generated/default/`, and this repo supplies the host app, function hooks, and an optional native renderer.

`assets/` (the game data, including `default.xex`, `default.xexp` and `assets/shaders`) and `generated/default/` are gitignored and never committed. `generated/rexglue.cmake` is SDK-generated: do not edit it.

## Build

CMake presets (Ninja + clang), e.g. `win-amd64-debug`, `win-amd64-release`, `win-amd64-relwithdebinfo`. The build dir is `out/build/<preset>`.

```
cmake --preset win-amd64-debug                          # xenos plugin build (default)
cmake --preset win-amd64-release -DSYLPHEED_NATIVE_GPU=ON   # native renderer build
cmake --build out/build/<preset>
```

For a second build dir with the other renderer, use a different `-B` (e.g. `-B out/build/win-amd64-debug-native`). There are no tests; you verify a change by running the game.

Useful targets and options:
- `sylpheed_codegen` reruns recompilation, which normally runs as part of the build whenever an input in `codegen.d` changes. Codegen applies `assets/default.xexp` (title update 0.0.1.2) automatically, so guest addresses are the *patched* ones.
- `sylpheed_shader_hlsl_dump` (native only) writes the recompiled HLSL, with names, to `<build>/hlsl_dump`.
- `SYLPHEED_NATIVE_GPU` (default OFF) is a link-time switch, not a cvar. OFF uses the SDK's xenos GPU plugin (Xenia-style emulation). ON replaces the guest D3D library with the plume renderer. On Windows that builds `sylpheed.exe` (D3D12) and `sylpheed_vk.exe` (Vulkan), and both link the same `sylpheed_recomp` objects.
- `SYLPHEED_OPTIMIZE_RECOMP` (default ON) compiles the recompiled guest code at -O2 even in Debug. At -O0 the VMX128 emulation helpers make the game unplayably slow.

To run, start the exe from the build dir. Game data is found at `assets/` next to the config, or set with `--game_data_root=<dir>`. Cvars are passed as `--name=value`. The project's own cvars are all prefixed `sylpheed_` (find them with `grep REXCVAR_DEFINE src`).

## Architecture

**Manifest → codegen → hooks.** `sylpheed_manifest.toml` includes `sylpheed_config.toml` (extra function entry points that analysis misses) and `config/{d3d,xapi,game}.toml`. Those name guest functions at fixed addresses, and host code replaces them by name with `REX_HOOK(GuestName, host_fn)`. To hook a new guest function, first name its address in the right TOML, then add the hook. `config/d3d.toml` also records the D3DDevice layout, and `src/gpu/d3d.h` holds the guest struct layouts (XDK 2.0.3215). Check both before re-deriving any address or offset.

**App entry.** `src/main.cpp` → `SylpheedApp` (`src/sylpheed_app.h`), which overrides `rex::ReXApp` hooks. In native builds it nulls the SDK graphics, creates the plume device in `OnPreLaunchModule`, and installs the ImGui overlay drawer.

**Native renderer (`src/gpu`, only built with `SYLPHEED_NATIVE_GPU`).** Ported from rr6, which got it from reblue (Blue Dragon). It translates Xenos state to plume (D3D12/Vulkan).
- `gpu/hooks/*.cpp` replace the guest D3D API (device, draw, resource, shader, state). The guest's command ring is pointed at discard memory, so the host renders from the D3D calls themselves, never from GPU packets.
- Shaders: `scripts/extract_shaders.py` collects containers from `hidden/resource3d/Common.xpr` plus runtime dumps (`<build>/user/shaders`, `assets/shaders`). `thirdparty/XenosRecomp` (submodule, branch `sylpheed`, built with `REBLUE_RECOMP RR6_RECOMP`) recompiles them to DXIL/SPIR-V in the generated `shader_cache.cpp` (`cmake/shader_cache.cmake`). For D3D12, `sylpheed_prelink` DXC-links every spec-constant variant at build time. Shaders compiled from embedded HLSL or stored in XEX `.rdata` can only be captured at run time with `--sylpheed_shader_dump`, and a fresh build drops their draws unless the dumps are in `assets/shaders`.
- `thirdparty/plume` is a submodule (branch `rr6`). Host-side helper shaders are built through `cmake/shaders.cmake` (`sylpheed_host_shader`).
- Guest memory access goes through `sylpheed::mem::at<T>(va)` (`core/memory_helpers.h`). Do not write raw `membase + va`: it skips the 0xE0-heap compensation. Guest fields are big-endian (`be_u32` etc.). Logging uses the `SYLPHEED_*` macros (`core/logging.h`).

**Shared test tooling (both builds).** `src/platform/input_script.cpp` provides `--sylpheed_input_script`. `"mission"` plays from boot into mission 1, and entries look like `button@frame[+hold]`, where `s` means seconds since the first pad poll and `w` means seconds since the first 3D frame. `dump_schedule.h` parses `--sylpheed_dump_frames` (frame numbers or `Nw` mission seconds). It drives the native frame dump and, in the xenos build, `xenos_resolve_dump.cpp`. `--sylpheed_renderdoc_at_mission=N` triggers a RenderDoc capture. Mission start is the first call of `Sylpheed_RenderScene` (0x822CD4E8, `config/game.toml`). Use `w` times when comparing the two builds, because their frame counts differ.

## Conventions

- Treat any assumption inherited from rr6 or reblue as unverified until Sylpheed's own code confirms it. Fuzzy matching against rr6 has mislabeled functions before, so check every match against the function body.
- Comments cite the guest addresses and offsets they rely on (see `d3d.h`, `d3d.toml`). Keep doing that.
