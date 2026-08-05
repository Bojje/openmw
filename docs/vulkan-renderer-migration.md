# Vulkan renderer migration plan

This plan deliberately prioritizes removal of duplicate renderer code. OpenSceneGraph (OSG) remains available only as a reference implementation while Vulkan is being brought to parity; it is not kept alive through a permanent compatibility layer.

## Working rules

1. Preserve the current OSG implementation in Git (a tag or reference worktree), not in the active Vulkan execution path.
2. Every new abstraction must replace an existing responsibility or have a concrete deletion milestone.
3. Vulkan and OSG must never both initialize or render in one process.
4. A deletion checkpoint must compile, pass fast tests, and pass the deterministic renderer smoke test before the next subsystem is removed.
5. Visual parity is the minimum bar; performance and visual improvements come after parity.

## Stages

### 1. Freeze the reference and establish measurements

- Tag or create a reference worktree from the current OSG build.
- Record build results, startup/shutdown behavior, representative screenshots, frame time, and source line counts.
- Map OSG ownership of world rendering, GUI, loading screens, screenshots, resources, stereo, and post-processing.
- Maintain a deletion ledger listing each OSG subsystem and its Vulkan replacement.

### 2. Create a deterministic renderer-test foundation

- Add a small test mode or executable that starts one renderer, loads a manifest of test scenes/cameras, renders multiple checkpoints, writes images, and exits.
- Use fixed camera paths, time, weather, random seed, resolution, and content.
- Add CPU-side tests for matrix conversion, NIF conversion, transforms, resource lookup, and scene snapshots.
- Compare Vulkan output with OSG reference images using tolerances rather than exact pixel equality.

### 3. Remove the dual-renderer lifecycle

- Initially use `OPENMW_USE_VULKAN` as a compile-time backend choice.
- In Vulkan mode, create one Vulkan window and skip OSG window/context initialization.
- Keep the default OSG build unchanged until the Vulkan path owns the required engine services.
- Add runtime backend selection only after the backend boundary is stable.

### 4. Make Vulkan frame infrastructure correct

- Correct acquire/present semaphore ownership.
- Keep acquire semaphores and fences per frame-in-flight, but presentation semaphores per swapchain image.
- Handle minimized windows and zero drawable sizes without recreating a zero-sized swapchain.
- Make swapchain recreation, image layout transitions, validation layers, and resource lifetime testable.

### 5. Replace OSG scene ownership

- Separate cell visibility, transforms, camera state, lighting, and material data from OSG scene nodes.
- Feed both reference and Vulkan implementations from renderer-neutral scene data during the transition.
- Delete OSG scene ownership once Vulkan consumes all required scene events.

### 6. Port static world rendering

- Wire NIF loading and `MeshConverter` into resource management.
- Implement model caching, cell add/remove, transforms, textures, materials, terrain, interiors, and static objects.
- Reach a static playable scene without OSG rendering.

### 7. Port dynamic content and presentation

- Add actors, skinning, animation, particles, weather, water, spell effects, and post-processing.
- Port GUI, fonts, loading screens, cursor handling, screenshots, and video presentation.
- Keep ray tracing optional until TLAS creation and the ray-tracing pipeline are complete.

### 8. Compare and delete

- Compare correctness, visual output, startup time, frame time, memory use, and mod compatibility.
- Delete duplicate adapters, dead OSG paths, obsolete Vulkan stubs, and transitional interfaces.
- Update the deletion ledger and line-count report after every subsystem removal.

### 9. Decide the final OSG policy

- Keep OSG as a separate compatibility/reference build if it remains valuable.
- Make Vulkan the default only after feature and CI coverage are sufficient.
- Remove OSG dependencies from the active build only when the fallback is no longer needed.

## Test cadence

Normal development should use this order:

1. Compile and static checks.
2. CPU/unit tests.
3. One deterministic renderer-test process covering many scenes and checkpoints.
4. One validation-layer run.
5. A small number of full-game startup, save/load, GUI, and gameplay smoke tests.

The full game should not be repeatedly started for every change.
