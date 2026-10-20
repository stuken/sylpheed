# sylpheed

sylpheed rebuilds Project Sylpheed: Arc of Deception (Xbox 360) as a native PC program using static recompilation. The game's PowerPC code is translated to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled for the host, so the game runs directly instead of inside an emulated console.

> [!IMPORTANT]
> This repository contains nothing from the game. You need the files from your own retail copy of Project Sylpheed, together with its title update, and both building and running need them. Everything derived from the game is produced on your machine at build time and is never committed:
> - the recompiled C++ that codegen generates from `default.xex` (`generated/default/`, gitignored);
> - the Xenos shaders taken from the game's `Common.xpr` or captured while it runs, and the DXIL and SPIR-V that XenosRecomp makes from them (inside the build directory);
> - the window icon, also taken from `default.xex` (`src/icon.generated.*`, gitignored).

## Renderers

sylpheed has two renderers. You pick one when you configure the build.

- **xenos plugin** (`SYLPHEED_NATIVE_GPU=OFF`, the default). This is the SDK's Xenia-derived GPU emulation. It is the reference that the native renderer is checked against.
- **Native renderer** (`SYLPHEED_NATIVE_GPU=ON`). This replaces the game's Direct3D library with a renderer built on [plume](https://github.com/renderbag/plume), ported from [rr6](https://github.com/stuken/rr6) and before that from [re:Blue](https://github.com/zolaware/reblue). The game's Xenos shaders are recompiled at build time with [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and embedded in the executable. On Windows this builds two executables from one tree:
  - `sylpheed.exe`: Direct3D 12.
  - `sylpheed_vk.exe`: Vulkan.

## Platform status

| Platform | State |
|---|---|
| Windows x64 | Builds and runs, with both renderers and both native backends. |
| Linux x64 | CMake support is carried over from rr6 but has not been built yet. It would use the Vulkan backend. |
| macOS | Not ported. |

## Building from scratch (Windows)

The build has three stages:

1. Clone sylpheed, which brings the ReXGlue SDK with it, and add your game files.
2. Build and install the SDK.
3. Configure, run codegen, and build.

The first build takes a while. Codegen turns the whole executable into C++, and there is a lot of it to compile.

### 1. Prerequisites

- **Visual Studio 2022** (Community is fine) with these components:
  - Desktop development with C++.
  - C++ Clang Compiler for Windows. sylpheed and the SDK build with clang, not MSVC.
  - C++ CMake tools for Windows. This provides CMake and Ninja; standalone CMake 3.25+ and Ninja also work.
  - A Windows SDK. 10.0.26100 or later is recommended: with older headers the D3D12 backend bakes depth bias into pipelines instead of setting it per draw. That path works, but it compiles more pipelines.
- **Git**.
- **Python 3**, for the native renderer's shader extraction. It uses only the standard library.

No Vulkan SDK or DirectX Shader Compiler install is needed. XenosRecomp vendors DXC for Windows and Linux.

Run every command below from an **x64 Native Tools Command Prompt for VS 2022**, with VS's clang on `PATH`:

```bat
set "PATH=%VCINSTALLDIR%Tools\Llvm\x64\bin;%PATH%"
clang --version
```

### 2. Clone sylpheed and add the game files

```bat
git clone --recursive https://github.com/stuken/sylpheed.git
cd sylpheed
```

`--recursive` fetches the submodules: `thirdparty/rexglue-sdk`, `thirdparty/plume` and `thirdparty/XenosRecomp`, along with their own dependencies. The SDK's dependencies make this a large download. If you cloned without it, run:

```bat
git submodule update --init --recursive
```

Extract the contents of your Project Sylpheed disc into `assets/`, using a tool such as extract-xiso, so that you end up with `assets/default.xex` next to the `dat` and `hidden` folders. Then put the title update's `default.xexp` next to `default.xex`. `assets/` is gitignored.

The game always runs patched: the SDK applies `default.xexp` when it loads `default.xex`, for codegen and at run time. The function addresses in `sylpheed_config.toml` and `config/` are specific to this pair of files:

| | |
|---|---|
| Title ID | `535107D4` |
| Title update | 0.0.1.2 |
| `default.xex` MD5 | `73ba9acd88426fb802111a677a6ab527` |
| `default.xexp` MD5 | `bc4698033fb8a0787eb2df33aa2ac782` |

Check your copy against these. A different `default.xex`, or a missing or different `default.xexp`, will not recompile correctly against these configs.

### 3. Build and install the ReXGlue SDK

sylpheed needs SDK version 0.10.0. It is developed against [stuken/rexglue-sdk](https://github.com/stuken/rexglue-sdk), which adds GPU, kernel and XAM fixes on top of upstream, and pins it as `thirdparty/rexglue-sdk`. From the sylpheed folder:

```bat
cd thirdparty\rexglue-sdk
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config RelWithDebInfo --target install
cd ..\..
```

This installs the SDK to `thirdparty\rexglue-sdk\out\install\win-amd64`, which step 4 points sylpheed at. The SDK uses a multi-config generator. Install each configuration you plan to build sylpheed in: run the build command again with `--config Release` or `--config Debug`.

An SDK built elsewhere works too: point `CMAKE_PREFIX_PATH` at its install instead, or set `REXSDK_DIR` to an SDK source tree to build it as part of sylpheed.

### 4. Configure, run codegen, and build

Set `CMAKE_PREFIX_PATH` to the SDK install from step 3. Add `-DSYLPHEED_NATIVE_GPU=ON` for the native renderer, or leave it off for the xenos plugin. Giving each its own build directory keeps both:

```bat
set REXSDK=%CD%\thirdparty\rexglue-sdk\out\install\win-amd64
set BUILD=out\build\native-relwithdebinfo

cmake --preset win-amd64-relwithdebinfo -B %BUILD% -DCMAKE_PREFIX_PATH=%REXSDK% -DSYLPHEED_NATIVE_GPU=ON
cmake --build %BUILD% --target sylpheed_codegen
cmake --preset win-amd64-relwithdebinfo -B %BUILD%
cmake --build %BUILD%
```

The commands do the following:

1. **Configure.**
2. **Codegen.** The SDK's `rexglue codegen` recompiles the patched `default.xex` into `generated/default/`, following `sylpheed_manifest.toml`.
3. **Reconfigure.** CMake only picks up the generated sources on a configure that runs after they exist. Without this step the first build fails to link. Later builds re-run codegen by themselves when its inputs change, so you need the reconfigure only once per fresh tree.
4. **Build.** This builds everything. With the native renderer that includes the shader cache:
   - `scripts/extract_shaders.py` gathers the 146 shader containers in `hidden/resource3d/Common.xpr`, and any shaders captured at run time (see below).
   - XenosRecomp turns them into DXIL and SPIR-V, and `sylpheed_prelink` links every spec-constant variant for D3D12.
   - The result is compiled into the executable.

Debug and Release presets work the same way (`win-amd64-debug`, `win-amd64-release`). The output goes to the build directory, along with the DLLs it needs from the SDK and DXC:

- With `SYLPHEED_NATIVE_GPU=ON`: `sylpheed.exe` (D3D12) and `sylpheed_vk.exe` (Vulkan).
- With it off: `sylpheed.exe`, using the xenos plugin.

These options apply to the build:

| Option | Default | Effect |
|---|---|---|
| `SYLPHEED_NATIVE_GPU` | `OFF` | Native renderer instead of the xenos plugin. |
| `SYLPHEED_D3D12` | `ON` on Windows | `sylpheed` uses D3D12. With `OFF`, `sylpheed` itself uses Vulkan. |
| `SYLPHEED_VULKAN_EXE` | `ON` | Also build `sylpheed_vk` (Windows). |
| `SYLPHEED_OPTIMIZE_RECOMP` | `ON` | Compile the recompiled guest code at `-O2` even in Debug builds. Unoptimized, the SDK's VMX128 helpers make the game far too slow to play. |
| `SYLPHEED_SHADER_DUMP_DIRS` | `<build>\user\shaders;assets\shaders` | Directories of captured shaders to recompile. |

### Shaders captured at run time

Not every shader can be read at build time. A few sit in `default.xex`'s compressed `.rdata` (the movies' colour conversion and some 2D passes), and the game compiles a few more at run time from HLSL embedded in the executable (the debug font and primitive drawing). A fresh native build has no recompiled code for these 13 shaders and skips the draws that use them.

The native renderer writes each shader it has no recompiled entry for to `user\shaders` next to the executable (`sylpheed_shader_dump`, on by default). The next build of the same build directory picks them up, so play through the boot movies and into a mission once, then build again. Copy the `.vso` and `.pso` files into `assets\shaders` to have every build directory include them.

## Running

`sylpheed.exe` looks for the game in `assets\default.xex` next to itself. Either copy or link your `assets` folder into the build directory, or point the game at it:

```bat
out\build\native-relwithdebinfo\sylpheed.exe --game_data_root="C:\path\to\sylpheed\assets"
```

Next to the executable, sylpheed uses:

- `sylpheed.toml`: the config file. Any cvar can go there, and the same cvar can be given on the command line as `--name=value`.
- `user\`: saves, the gamer profile, captured shaders, frame dumps and texture dumps.
- `cache\`: with `sylpheed_pso_capture`, the captured pipeline list.

### Settings

| Cvar | Default | Effect |
|---|---|---|
| `sylpheed_lod_scale` | `1` | Multiplies the distances at which models switch to lower-detail meshes. Above `1` keeps detailed meshes farther out, `0` always uses the most detailed. Works with both renderers. |

These apply to the native renderer:

| Cvar | Default | Effect |
|---|---|---|
| `sylpheed_resolution_scale` | `1` | Integer render scale over the game's 1280x720. `0` picks one from the output height, `1` is native. |
| `sylpheed_supersampling` | `1` | `2` renders at twice the resolution scale and downsamples at present. Restart to apply. |
| `sylpheed_anisotropy` | `default` | `default` leaves filtering as the game's sampler states set it; `2`, `4`, `8` or `16` forces that level on every linearly filtered sampler. |
| `sylpheed_fps_limit` | `60` | Frame rate cap, `0` for none. |
| `sylpheed_vsync` | `true` | Vertical sync. |
| `sylpheed_pso_precache` | `true` | Compile the recorded pipelines in the background before draws need them. |
| `sylpheed_display_correction` | `true` | Apply the console's HDTV output curve at present, as its D3D programs it. `false` shows the raw frame, with brighter mid-tones. |
| `sylpheed_output_16bit` | `true` | Keep the game's back buffer at 16 bits per channel and dither at present, instead of the console's 8 bits. Restart to apply. |
| `sylpheed_scaled_post_filters` | `true` | At a resolution scale above 1, upscale the 320x180 glow smoothly where full-scale passes sample it. |
| `sylpheed_texture_dump` | `false` | Write each texture the game uses to `user\textures\dump` as a DDS file, once. |
| `sylpheed_texture_replace` | `true` | Use DDS files in `user\textures\replace` in place of the game's textures they match. |

### Testing and diagnostics

These are for working on the port rather than playing.

| Cvar | Effect |
|---|---|
| `sylpheed_input_script` | Scripted pad input, `button@frame[+hold],...`. A trailing `s` counts seconds from the first pad poll and `w` seconds from the mission's first 3D frame. `mission` plays from boot into the first mission. Both builds. |
| `sylpheed_null_audio` | Discard audio output, for machines without an audio device. |
| `sylpheed_dump_frames` | Present frames to write as BMPs to `user\`, by frame number or as `Nw` mission seconds. In the xenos build it dumps resolve targets; add `--readback_resolve=full --d3d12_readback_resolve=true`. |
| `sylpheed_trace_frame`, `sylpheed_trace_at` | Log every draw and resolve of a present frame, or of the frame this many seconds into the mission. |
| `sylpheed_renderdoc_at_mission` | Under RenderDoc, capture the frame this many seconds into the mission. Both builds. |
| `sylpheed_perf_stats`, `sylpheed_perf_gpu_draws` | Log frame times and per-pass renderer and GPU timings every N presents, optionally per draw. |
| `sylpheed_pso_capture` | Write the precache list plus every pipeline a draw had to compile to `cache\pipeline_state_cache.h`, ready to replace `src\gpu\pipeline\cache\pipeline_state_cache.h`. |
| `sylpheed_shader_dump` | Write shaders missing from the recompiled cache to `user\shaders` (on by default). |
| `sylpheed_screenshot_every` | Write every Nth presented frame to `user\screens`. |
| `sylpheed_sample_threads` | Log every guest thread's call chain every N vblanks. |
| `sylpheed_devmode` | Extra renderer diagnostics and developer logging. |
| `sylpheed_d3d12_debug`, `sylpheed_dred` | D3D12 debug layer, and the breadcrumbs that name the operation a lost device died on. |
| `sylpheed_baked_depth_bias` | Force the older-Windows path that bakes depth bias into pipelines. |

## Repository layout

| Path | Contents |
|---|---|
| `sylpheed_manifest.toml`, `sylpheed_config.toml`, `config/` | Codegen inputs: the entry point, extra function entry points, and the named XDK 2.0.3215 Direct3D, XAPI and game functions that the hooks replace. |
| `generated/rexglue.cmake` | SDK glue. `generated/default/` holds the codegen output and is gitignored. |
| `src/sylpheed_app.h`, `src/main.cpp` | App entry point and path setup. |
| `src/gpu/` | The native renderer: D3D hooks, resource mirrors, pipelines, the frame ring, and presentation. |
| `src/game/` | Game-logic hooks both renderers share. |
| `src/platform/` | Input scripting, silent audio, the xenos build's resolve dumps and RenderDoc trigger, and window and threading helpers. |
| `cmake/` | The native renderer, shader cache and host shader build. |
| `scripts/` | Shader extraction. |
| `tools/` | `sylpheed_icon`, which takes the window icon from `default.xex`, and `sylpheed_prelink`. |
| `thirdparty/` | The ReXGlue SDK fork, plume (on its `rr6` branch), XenosRecomp (on its `sylpheed` branch, which adds point sprites and the Xenos piecewise-linear texture gamma), miniz, zstd and the RenderDoc API header. |

## License

sylpheed's own code is under the [BSD Zero Clause License](LICENSE), as rr6's is: use it for anything, with no conditions. The code ported from re:Blue, by way of rr6, stays under re:Blue's BSD 3-Clause License ([licenses/reblue-BSD-3-Clause.txt](licenses/reblue-BSD-3-Clause.txt)), as its file headers say. The submodules and `thirdparty/` keep their own licenses. No license here covers Project Sylpheed itself.

## Credits

- [re:Blue](https://github.com/zolaware/reblue), whose native renderer this one is ported from, by way of rr6.
- [rr6](https://github.com/stuken/rr6), the Ridge Racer 6 port this one starts from.
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) team, for the recompilation toolchain.
- [hedge-dev](https://github.com/hedge-dev), for [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp).
- [plume](https://github.com/renderbag/plume), the rendering hardware interface.
- The [Xenia](https://github.com/xenia-project/xenia) project and the wider Xbox 360 emulation scene.
