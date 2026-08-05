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
The pre-migration OSG reference is frozen at the `openmw-vulkan-osg-reference` tag.

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
smoke path now resolves all loaded-cell snapshots through the cached NIF meshes, filters paged
objects by neutral visibility, and composes object transforms with NIF node transforms before batching.
NIF classic texture, diffuse/emissive, glossiness, and alpha properties now cross the
renderer-neutral mesh boundary and survive batching; the neutral batch applies diffuse
and alpha to vertex color output. Resource images can now cross into neutral RGBA8 data,
and the standalone Vulkan renderer uploads/caches indexed albedo textures and samples them
in the G-buffer (currently bounded to a 64-entry table). Vulkan now consumes neutral
alpha-test state and thresholds in the G-buffer
cutout path, and the standalone harness has a basic source-alpha pipeline for blended draws;
deferred ordering, full material shading, and full-game resource hookup remain outstanding.
The Vulkan G-buffer now carries neutral roughness,
ambient-occlusion,
and emissive-strength channels into the composite pass. Cell object lookup and removal are
owned by the renderer-neutral `WorldScene`/`CellScene` components rather than the OSG-facing
manager; the manager now only translates engine lifecycle events into that component. The
manager also exposes a renderer-neutral camera/inverse-matrix and directional-light snapshot
for a future Vulkan frame consumer. Neutral loaded-cell snapshots retain insertion order when
collected, making backend draw lists stable for image comparison and predictable alpha ordering.
The Vulkan composite pass now consumes that single scene-lighting UBO directly; duplicated
sun push constants were removed, and ambient light is part of the neutral snapshot. The
OSG-facing manager now also exposes neutral loaded-world mesh collection and RGBA8 texture
resolution backed by the existing resource caches, giving a future Vulkan consumer a concrete
full-game input without exposing OSG objects.
Those inputs can now be collected as one `Render::SceneSubmission`; `Vk::Renderer::setScene`
consumes that handoff in the standalone path, and the smoke test exercises it. The full-game
Vulkan call site is still intentionally absent until window, input, dynamic-content, and GUI
services have a Vulkan owner.
The fast test suite now also contains a backend-neutral RGBA8 image comparator with
per-channel tolerance, differing-pixel count, maximum error, and mean error metrics.
The Vulkan smoke path now reads back rendered RGBA8/BGRA8 swapchain frames and compares
consecutive captures with that comparator when a presentation-capable host is available;
the local headless environment still skips before this runtime path. This makes future
OSG/Vulkan captures diagnosable instead of reducing them to an opaque pixel mismatch.
A neutral terrain tile snapshot adapter also converts the legacy
OSG-array/OSG-image storage contract into vertices, layer metadata, and RGBA8 blendmaps;
opaque single-layer terrain retains an intentionally absent blendmap.

Against the actual PR base `origin/openmw-vulkan` (PR #5), the current checkpoint changes
45 files, deleting 250 lines and adding 2,817 lines (net `+2,567`). The larger Vulkan-only
cleanup was completed in the merged PRs #1–#5; this PR is currently a groundwork expansion,
not the speculative 10k-line reduction. Further deletion must wait for a live Vulkan
consumer to replace the remaining OSG-owned responsibilities.

The latest reduction checkpoint also removed two unused `RenderingManager` wrappers: the
neutral cell lookup and the unconsumed manager-level terrain snapshot. The terrain adapter
remains available at `TerrainStorage`, where its conversion is directly tested, until a
Vulkan terrain consumer gives it a real owner.

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
| NIF-to-neutral mesh conversion | Renderer-neutral NIF boundary, material data, mesh cache, `SceneSubmission`, Vulkan mesh batch, standalone texture table, and full-game neutral resolver | Connect the handoff to the live full-game Vulkan frame loop, add material shading, skinning, and static-world submission |
| Terrain geometry and layer data | Legacy OSG terrain storage/ChunkManager plus a tested renderer-neutral tile snapshot adapter | Add a Vulkan terrain consumer, terrain paging/LOD policy, and terrain image/shader coverage |
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

- Add a small test mode or executable that starts one renderer, loads a manifest of test scenes/cameras, renders multiple checkpoints, writes images, and exits. The current standalone smoke target validates neutral mesh/cache/material setup before window creation, then covers one textured alpha-blended scene, reads back each rendered swapchain frame, and compares consecutive captures when a Vulkan surface is available; persistent image files and OSG reference comparison remain pending.
- Use fixed camera paths, time, weather, random seed, resolution, and content.
- Add CPU-side tests for matrix conversion, NIF conversion, transforms, resource lookup, and scene snapshots. The current fast tests cover matrix conversion, NIF conversion, parent-child transforms, safe index handling, cache reuse, cell-object transform composition, and renderer-neutral batch layout.
- Compare Vulkan output with OSG reference images using the neutral image comparator's
  tolerances and error metrics rather than exact pixel equality. Vulkan-to-Vulkan
  capture comparison is now wired into smoke; OSG reference-image execution remains
  pending until a presentation-capable validation host and reference capture workflow
  are available.

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

- Separate cell visibility, transforms, camera state, lighting, and material data from OSG scene nodes. Camera/light scene data now has an OSG-to-neutral snapshot source, alongside transform-preserving neutral mesh instances, updateable `WorldScene`/`CellScene` snapshots, explicit paged-reference visibility, neutral NIF material extraction, a cached-mesh cell composition adapter, a concrete `SceneSubmission` handoff, and standalone Vulkan texture/alpha consumption; the live full-game backend call site and complete shading remain to be migrated.
- Feed both reference and Vulkan implementations from renderer-neutral scene data during the transition.
- Delete OSG scene ownership once Vulkan consumes all required scene events.

### 6. Port static world rendering

- Wire NIF loading and `MeshConverter` into resource management. The NIF converter now has a tested tree traversal and material boundary, `NifMeshManager` caches converted instances, image resources expose neutral RGBA8 data, and `RenderingManager` can collect a `SceneSubmission` without exposing OSG objects. The standalone Vulkan path consumes that submission and its resolved textures; a live full-game Vulkan frame consumer, shading, and a static-world consumer are still outstanding.
- Implement model caching, cell add/remove, transforms, textures, materials, terrain,
  interiors, and static objects. The terrain adapter is now the boundary for the
  geometry/layer portion; Vulkan upload, terrain paging/LOD, and terrain shaders remain.
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
