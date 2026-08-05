# Vulkan renderer migration plan

This plan deliberately prioritizes removal of duplicate renderer code. OpenSceneGraph (OSG) remains available only as a reference implementation while Vulkan is being brought to parity; it is not kept alive through a permanent compatibility layer.

## Working rules

1. Preserve the current OSG implementation in Git (a tag or reference worktree), not in the active Vulkan execution path.
2. Every new abstraction must replace an existing responsibility or have a concrete deletion milestone.
3. Vulkan and OSG must never both initialize or render in one process.
4. A deletion checkpoint must compile, pass fast tests, and pass the deterministic renderer smoke test before the next subsystem is removed.
5. Visual parity is the minimum bar; performance and visual improvements come after parity.

## Current checkpoint

The experiment is currently isolated on the `openmw-vulkan` branch. The full game remains
OSG-only, and the Vulkan renderer is exercised by the standalone smoke target; this keeps
the process lifecycle single-backend while the scene bridge is incomplete.

Completed reduction checkpoints include removal of the incomplete full-game Vulkan bridge,
the unused Vulkan mesh submission queue, inactive ray-tracing scaffolding, and unused
buffer, descriptor, command-helper, compute, and transfer-queue paths. The current bridge
also contains renderer-neutral scene/math data, a validated NIF triangle conversion path,
transform-preserving NIF tree traversal for static mesh discovery, and a Vulkan draw of the
resulting neutral mesh. Renderer-neutral batching now flattens multiple mesh instances with
independent transforms before the Vulkan backend uploads them. Parsed NIF resources now use a shared-pointer cache instead of an OSG object
wrapper, and converted renderer-neutral NIF mesh instances have a separate path-keyed
cache owned by `ResourceSystem`. The smoke harness exercises that cache boundary before
submitting its test mesh to Vulkan. Loaded world references now also have a renderer-neutral
cell snapshot: the scene lifecycle records model identity, position, orientation, scale,
visibility, cell transfer, and removal independently of the OSG node tree. Paged references
remain in the snapshot with `visible == false` until the scene activates them. OSG still consumes the same
events, but it no longer needs to be the only source of object transform state. The Vulkan
smoke path now resolves a cell snapshot through the cached NIF meshes, filters paged objects
by neutral visibility, and composes object transforms with NIF node transforms before batching.
NIF classic texture, diffuse/emissive, glossiness, and alpha properties now cross the
renderer-neutral mesh boundary and survive batching, while Vulkan texture binding and
material shading remain outstanding. Cell object lookup and removal are
owned by the renderer-neutral `WorldScene`/`CellScene` components rather than the OSG-facing
manager; the manager now only translates engine lifecycle events into that component.

The remaining migration is not a compatibility problem that can be solved by retaining
both renderers in one execution path. Static-world transforms, materials, textures,
terrain, dynamic content, GUI, and presentation still have OSG ownership. Those are the
next deletion prerequisites; deleting OSG before they have Vulkan consumers would leave
the game unplayable rather than reduce duplication safely.

### Deletion ledger

| Responsibility | Current owner | Deletion condition |
| --- | --- | --- |
| Full-game scene graph and world rendering | OSG | Vulkan static and dynamic scene consumers reach parity |
| Vulkan validation renderer | Vulkan standalone smoke target | Retained as the migration test harness |
| Vulkan mesh submission queue | Removed | Complete |
| Inactive raster ray-tracing scaffold | Removed | Reintroduce only with a complete RT pipeline |
| Vulkan utility/queue helper paths | Removed | Complete |
| Parsed NIF resource cache wrapper | Removed | Complete; cache now owns shared NIF files directly |
| NIF-to-neutral mesh conversion | Renderer-neutral NIF boundary, material data, mesh cache, and Vulkan mesh batch | Add texture binding, material shading, skinning, and static-world submission |
| Loaded-cell object identity, transforms, and paging state | Renderer-neutral `WorldScene`/`CellScene` snapshots updated by scene lifecycle | Consume snapshots from a backend and migrate visibility/paging policy |
| GUI, loading screens, screenshots, and presentation | OSG/MyGUI path | Vulkan presentation and GUI coverage |

This ledger is intentionally conservative: a subsystem is marked removable only after a
real replacement consumes its responsibility and the fast tests cover the boundary.

## Stages

### 1. Freeze the reference and establish measurements

- Tag or create a reference worktree from the current OSG build.
- Record build results, startup/shutdown behavior, representative screenshots, frame time, and source line counts.
- Map OSG ownership of world rendering, GUI, loading screens, screenshots, resources, stereo, and post-processing.
- Maintain a deletion ledger listing each OSG subsystem and its Vulkan replacement.

### 2. Create a deterministic renderer-test foundation

- Add a small test mode or executable that starts one renderer, loads a manifest of test scenes/cameras, renders multiple checkpoints, writes images, and exits. The current standalone smoke target covers one neutral mesh and multiple frame submissions; image capture and reference comparison remain pending until a Vulkan-capable presentation or offscreen test target is available.
- Use fixed camera paths, time, weather, random seed, resolution, and content.
- Add CPU-side tests for matrix conversion, NIF conversion, transforms, resource lookup, and scene snapshots. The current fast tests cover matrix conversion, NIF conversion, parent-child transforms, safe index handling, cache reuse, cell-object transform composition, and renderer-neutral batch layout.
- Compare Vulkan output with OSG reference images using tolerances rather than exact pixel equality.

### 3. Remove the dual-renderer lifecycle

- Initially use `OPENMW_USE_VULKAN` as a compile-time backend choice.
- Delete the incomplete second-window Vulkan bridge from the full game until Vulkan owns the required engine services.
- Keep the full game on one OSG renderer and keep Vulkan validation in the standalone renderer smoke target during the scene-bridge phase.
- When the renderer-neutral scene bridge is ready, create one Vulkan window and skip OSG window/context initialization in Vulkan mode.
- Keep the default OSG build unchanged until the Vulkan path owns the required engine services.
- Add runtime backend selection only after the backend boundary is stable.

### 4. Make Vulkan frame infrastructure correct

- Correct acquire/present semaphore ownership. The standalone renderer now uses per-frame acquire semaphores, per-swapchain-image presentation semaphores, and per-image in-flight fence ownership.
- Keep acquire semaphores and fences per frame-in-flight, but presentation semaphores per swapchain image.
- Handle minimized windows and zero drawable sizes without recreating a zero-sized swapchain.
- Make swapchain recreation, image layout transitions, validation layers, and resource lifetime testable.

### 5. Replace OSG scene ownership

- Separate cell visibility, transforms, camera state, lighting, and material data from OSG scene nodes. Camera/light scene data, transform-preserving neutral mesh instances, updateable `WorldScene`/`CellScene` snapshots, explicit paged-reference visibility, neutral NIF material extraction, and a cached-mesh cell composition adapter are in place; Vulkan texture/shading consumption remains to be migrated.
- Feed both reference and Vulkan implementations from renderer-neutral scene data during the transition.
- Delete OSG scene ownership once Vulkan consumes all required scene events.

### 6. Port static world rendering

- Wire NIF loading and `MeshConverter` into resource management. The NIF converter now has a tested tree traversal and material boundary, `NifMeshManager` caches converted instances, and the loaded-cell lifecycle records neutral model/transform state; Vulkan texture/shading consumption and a static-world consumer are still outstanding.
- Implement model caching, cell add/remove, transforms, textures, materials, terrain, interiors, and static objects.
- Reach a static playable scene without OSG rendering.

### 7. Port dynamic content and presentation

- Add actors, skinning, animation, particles, weather, water, spell effects, and post-processing.
- Port GUI, fonts, loading screens, cursor handling, screenshots, and video presentation.
- Reintroduce ray tracing only after TLAS creation and the ray-tracing pipeline are complete; do not carry an inactive RT scaffold in the raster path.

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
