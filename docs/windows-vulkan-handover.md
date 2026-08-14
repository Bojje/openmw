# Windows Vulkan Migration Handover

## Objective

Continue the OpenMW OSG-to-Vulkan experiment on Windows without modifying the stable
OpenMW installation. The Windows Vulkan build should be staged separately in
`E:\OpenMW-Vulkan`.

## Important paths

| Purpose | Windows path | WSL path |
| --- | --- | --- |
| Stable installed OpenMW | `E:\OpenMW` | `/mnt/e/OpenMW` |
| Stable executable | `E:\OpenMW\openmw.exe` | `/mnt/e/OpenMW/openmw.exe` |
| New Vulkan staging/install directory | `E:\OpenMW-Vulkan` | `/mnt/e/OpenMW-Vulkan` |
| Windows source checkout | `E:\projects\openmw-vulkan` | `/mnt/e/projects/openmw-vulkan` |
| Linux/WSL source checkout (reference) | `\\wsl.localhost\Ubuntu-26.04\home\bojje\projects\openmw` | `/home/bojje/projects/openmw` |

`E:\OpenMW-Vulkan` currently exists and is empty. Keep `E:\OpenMW` untouched; it is the
working OSG installation and recovery point.

## Repository state and GitHub source

The active source branch is:

```text
openmw-vulkan
local HEAD: 492268f784
```

The source of truth for the Windows setup is the GitHub branch:

```text
https://github.com/Bojje/openmw.git
branch: openmw-vulkan
```

The current local branch contains the migration work that must be published to that branch
before the Windows checkout is created. After it is published, create the Windows source
checkout directly from GitHub:

```powershell
git clone --branch openmw-vulkan `
  https://github.com/Bojje/openmw.git `
  E:\projects\openmw-vulkan
```

If the checkout already exists, update it with:

```powershell
git -C E:\projects\openmw-vulkan fetch origin
git -C E:\projects\openmw-vulkan switch openmw-vulkan
git -C E:\projects\openmw-vulkan pull --ff-only origin openmw-vulkan
```

Before the Windows checkout is created, verify and publish the intended branch state from the
existing checkout:

```bash
git fetch origin openmw-vulkan
git log --oneline origin/openmw-vulkan..openmw-vulkan
git push origin openmw-vulkan
```

Do not reset the branch or merge it into `master` as part of this experiment.

Existing untracked benchmark fixtures belong to the user and must not be added, deleted, or
modified:

```text
files/data/benchmark.omwscripts
files/data/scripts/benchmark/
```

## Current migration status

The reduction/lifecycle portion is complete, but the full migration is not.

Completed or substantially implemented:

- OSG and Vulkan are mutually exclusive at runtime.
- Vulkan has its own frame owner and SDL window path.
- Neutral resource, NIF mesh, scene, cell/object, terrain, lighting, weather, projectile,
  effect, and animation data crosses into Vulkan without requiring OSG scene ownership.
- Vulkan raster G-buffer/composite rendering works for the standalone smoke scenes.
- Vulkan screenshots/readback and swapchain lifecycle have test coverage.
- Shared simulation profiling uses renderer-neutral `Render::FrameStats`.
- OSG resource release/statistics hooks are explicitly isolated from neutral cache contracts.

Still incomplete:

- GUI and MyGUI replacement
- Loading screens and modal/video presentation
- Full Vulkan input/presentation ownership
- Complete static-world and terrain parity
- Broader image-format/material coverage
- Remaining dynamic shading and visual effects
- Full-game visual comparison against OSG
- Removal of the Vulkan `NullWindowManager` compatibility surface

The Vulkan game entry point exists behind `--vulkan`, but it is explicitly experimental and
no-GUI. The default OSG build remains the playable game.

## Ray tracing status

Ray tracing is not implemented. The previous inactive ray-tracing scaffold was removed on
purpose. There is currently no complete:

- BLAS/TLAS implementation
- `VK_KHR_acceleration_structure` feature negotiation
- `VK_KHR_ray_tracing_pipeline` pipeline
- shader binding table
- RT descriptor/synchronization path
- RT fallback or screenshot test

Do not restore a partial RT scaffold. First make the raster Vulkan path usable as a full game
renderer. Then add one end-to-end RT feature, preferably ray-traced shadows with raster fallback,
using the existing neutral `SceneSubmission` as its geometry input.

## Windows build prerequisites

The current WSL environment does not contain a Windows compiler. A Windows build requires:

- Visual Studio 2022 or Visual Studio 2022 Build Tools
- Desktop C++ workload and Windows SDK
- Vulkan SDK
- `glslangValidator` available through the Vulkan SDK
- CMake and either the Visual Studio generator or Ninja
- The OpenMW Windows dependencies required by the normal build

Use a Windows-native source checkout for the Visual Studio build, preferably outside the
installed directory:

```text
E:\projects\openmw-vulkan
```

The WSL checkout is still visible from Windows at, but should not be the primary Windows build
source:

```text
\\wsl.localhost\Ubuntu-26.04\home\bojje\projects\openmw
```

A native Windows checkout is preferable for the Windows build because it avoids compiling a
large C++ project over the WSL UNC filesystem.

## Recommended Windows configuration

From a Visual Studio Developer PowerShell:

```powershell
cmake -S E:\projects\openmw-vulkan `
  -B E:\build\openmw-vulkan `
  -G "Visual Studio 17 2022" `
  -A x64 `
  -DOPENMW_USE_VULKAN=ON `
  -DBUILD_TESTING=ON `
  -DCMAKE_INSTALL_PREFIX=E:\OpenMW-Vulkan
```

Build and stage without touching the stable install:

```powershell
cmake --build E:\build\openmw-vulkan `
  --config RelWithDebInfo `
  --target openmw

cmake --install E:\build\openmw-vulkan `
  --config RelWithDebInfo
```

If the project dependencies are not found, configure their Windows dependency prefix through
the normal OpenMW build mechanism or `CMAKE_PREFIX_PATH`; do not copy Linux libraries into the
Windows staging directory.

## Windows validation order

Run these before attempting full gameplay:

```powershell
ctest --test-dir E:\build\openmw-vulkan -C RelWithDebInfo --output-on-failure
```

Then run the installed experimental executable from `E:\OpenMW-Vulkan` with the normal OpenMW
data/configuration and add:

```text
--vulkan
```

Expect no GUI and incomplete game presentation at this stage. Do not replace
`E:\OpenMW\openmw.exe`.

The Linux-side validation baseline is:

- Vulkan and OSG builds pass.
- Seven CPU/unit tests pass.
- Host-access windowed Vulkan smoke passes for three frames.
- Headless smoke currently skips on llvmpipe with `VK_ERROR_OUT_OF_DEVICE_MEMORY` during
  swapchain creation.
- Validation layers are not currently installed in the WSL environment.

## Recommended implementation sequence

1. Make the Windows Vulkan build configure, compile, install, and launch.
2. Fix Windows-specific Vulkan surface, GPU selection, shader, and DLL/resource lookup issues.
3. Verify the standalone smoke test on the real Windows GPU.
4. Make the Vulkan full-game path stable enough for one test cell and camera.
5. Replace the null GUI/loading/presentation services.
6. Complete raster static/dynamic/terrain parity and OSG comparison captures.
7. Add RT feature negotiation and a raster fallback.
8. Implement one complete RT vertical slice, preferably shadows.
9. Add RT smoke coverage and only then expand to reflections/global illumination.

## Architectural rules

- Never initialize OSG and Vulkan in the same runtime.
- Do not add OSG types to neutral simulation, scene, resource-cache, or Vulkan contracts.
- Do not preserve dead compatibility paths merely to avoid deleting them.
- Every new renderer abstraction must replace a named responsibility in the deletion ledger.
- Keep OSG as a separate reference/compatibility build until Vulkan reaches the documented parity
  gates.
- Treat CTest skips as environmental only when the test has proven that the host cannot provide
  a usable surface/device; renderer errors must remain failures.

## Key files

```text
docs/vulkan-renderer-migration.md
apps/openmw/engine.cpp
apps/openmw/mwworld/worldimp.cpp
apps/openmw/mwworld/scene.cpp
apps/openmw/mwgui/nullwindowmanager.*
components/render/
components/vk/
apps/vulkan_tests/renderer_smoke.cpp
apps/vulkan_tests/CMakeLists.txt
```

The migration ledger in `docs/vulkan-renderer-migration.md` is the authoritative list of
remaining OSG ownership and deletion prerequisites.
