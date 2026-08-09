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
OSG-only, and `OPENMW_USE_VULKAN` currently builds the standalone Vulkan migration renderer
and smoke tests; it is not yet a full-game backend selector. Vulkan translation units now
live in a separate `openmw_vulkan` library used by the migration targets instead of the
shared `components` archive, so the OSG game target does not link the inactive backend.
The top-level build now also rejects a future `openmw-lib -> openmw_vulkan` link, and CI
checks the final ELF dependencies and renderer-symbol set in both binaries, making that
separation a configure- and link-time invariant. This keeps the process lifecycle
single-backend while the scene bridge is incomplete. The standalone Vulkan smoke target
now links only SDL2 and Vulkan at runtime; no OSG library or renderer symbol is present.
Vulkan configuration also probes `glslangValidator --version` and rejects a missing or
no-op shader compiler, so a successful build cannot silently omit the SPIR-V artifacts.
Its fixture submits renderer-neutral mesh and terrain data directly, while the separate
renderer-mesh CPU test retains coverage for NIF conversion, material extraction, and the
path-keyed mesh cache. This makes the presentation validation process independent of the
legacy NIF/OSG object model.
The pre-migration OSG reference is frozen at the `openmw-vulkan-osg-reference` tag.
An independent OSG-only configuration using the repository's bundled Bullet, OSG, MyGUI,
and RecastNavigation dependencies has also been configured with `OPENMW_USE_VULKAN=OFF`
and rebuilt through the full `openmw` executable.

Completed reduction checkpoints include removal of the incomplete full-game Vulkan bridge,
the unused Vulkan mesh submission queue, inactive ray-tracing scaffolding, and unused
buffer, descriptor, command-helper, compute, and transfer-queue paths. The current bridge
also contains renderer-neutral scene/math data, a validated NIF triangle conversion path,
transform-preserving NIF tree traversal for static mesh discovery, and a Vulkan draw of the
resulting neutral mesh. Renderer-neutral batching now flattens multiple mesh instances with
independent transforms before the Vulkan backend uploads them. Parsed NIF resources now use a shared-pointer cache instead of an OSG object
wrapper, and converted renderer-neutral NIF mesh instances have a separate path-keyed
cache owned by `ResourceSystem`; its cache lifecycle is now forwarded explicitly without
the OSG `BaseResourceManager` interface. The renderer-mesh CPU test exercises that cache
boundary, while the presentation smoke submits neutral mesh instances directly. Loaded world references now also have a renderer-neutral
cell snapshot: the scene lifecycle records model identity, position, orientation, scale,
visibility, cell transfer, and removal independently of the OSG node tree. Paged references
remain in the snapshot with `visible == false` until the scene activates them. OSG still consumes the same
events, but it no longer needs to be the only source of object transform state. The neutral
world path resolves loaded-cell snapshots through cached NIF meshes, filters paged objects by
neutral visibility, and composes object transforms with NIF node transforms before batching;
animated objects are explicitly retained as dynamic snapshots and now also contribute converted
bind-pose geometry to the Vulkan batch as a fallback until a skinning/animation consumer owns
them; an ordered dynamic-object view provides that future backend with the retained visibility,
transform, model, and cell ordering, and each `SceneSubmission` copies those records across the
frame boundary. That handoff remains covered by the CPU tests until a live Vulkan game consumer
is connected.
NIF classic texture, diffuse/emissive, glossiness, and alpha properties now cross the
renderer-neutral mesh boundary and survive batching; the neutral batch applies diffuse
and alpha to vertex color output. NIF bump/normal texture slots now cross the same boundary
with generated or authored tangent frames for static mesh normal mapping. BSLighting
shader-authored alpha and BSShaderNoLighting texture properties are also preserved, so
static materials do not become opaque or textureless merely because their data arrived
through a Bethesda shader property rather than a classic NIF property. Classic NIF glow
slots and Bethesda shader glow textures now cross the same boundary and contribute to the
neutral emissive channel. Resource images can now cross into neutral RGBA8 data,
and authored BSLighting double-sided flags now select the matching Vulkan no-cull pipeline.
Classic and BS shader texture wrap flags now select per-resource repeat/clamp sampler variants.
The standalone Vulkan renderer uploads/caches indexed albedo textures and samples them
in the G-buffer (currently bounded to a 64-entry table). Vulkan now consumes neutral
alpha-test state and thresholds in the G-buffer
cutout path, and the standalone harness has a basic source-alpha pipeline for blended draws;
ordinary alpha draws are now sorted back-to-front with stable ties, while ordered terrain
layers retain their submission order; full material shading and full-game resource hookup
remain outstanding.
The Vulkan G-buffer now carries neutral roughness,
ambient-occlusion,
and emissive-strength channels into the composite pass. Cell object lookup and removal are
owned by the renderer-neutral `WorldScene`/`CellScene` components rather than the OSG-facing
manager; the manager now only translates engine lifecycle events into that component. The
manager also exposes a renderer-neutral camera/inverse-matrix and directional-light snapshot
for a future Vulkan frame consumer. Neutral loaded-cell snapshots retain insertion order when
collected, making backend draw lists stable for image comparison and predictable alpha ordering.
Fog color and start/end distances now cross the same neutral scene handoff, and the Vulkan
composite applies the active linear fog range after reconstructing world position.
The CPU ABI guard tracks the expanded 336-byte scene UBO so future neutral-state additions
cannot silently desynchronize the Vulkan shader layout.
The neutral world snapshot now has an explicit reset path owned by `RenderingManager::clear()`,
so a game/world unload cannot retain stale object identities, terrain tiles, or cell ordering.
The Vulkan composite pass now consumes that single scene-lighting UBO directly; duplicated
sun push constants were removed, and ambient light is part of the neutral snapshot. The
OSG-facing manager now also exposes neutral loaded-world mesh collection and RGBA8 texture
resolution backed by the existing resource caches, giving a future Vulkan consumer a concrete
full-game input without exposing OSG objects.
Those inputs can now be collected as one `Render::SceneSubmission`; `Vk::Renderer::setScene`
consumes that handoff in the standalone path, and the smoke test exercises it. The submission
boundary now validates mesh indices and terrain snapshots before Vulkan consumes them. The full-game
Vulkan call site is still intentionally absent until window, input, dynamic-content, and GUI
services have a Vulkan owner. Mesh submission no longer waits for the whole device or
rebuilds one global buffer: neutral mesh data is retained on the CPU and uploaded into
the current frame slot only after its fence is waited, so a future live frame loop can
submit scene updates without the previous device-wide stall.
Neutral cells now retain worldspace identity, and both mesh and terrain collection filter
to the active worldspace so an unloaded or inactive worldspace cannot leak into a Vulkan
submission.
The fast test suite now also contains a backend-neutral RGBA8 image comparator with
per-channel tolerance, differing-pixel count, maximum error, and mean error metrics.
The Vulkan smoke path now reads back rendered RGBA8/BGRA8 swapchain frames and compares
consecutive captures with that comparator when a presentation-capable host is available;
without a reference image it also replaces the scene once in the same process to exercise
descriptor growth and frame-safe mesh replacement;
an optional `OPENMW_VULKAN_HEADLESS=1` mode uses `VK_EXT_headless_surface` to exercise the
same frame lifecycle without SDL/X11/Wayland, omitting only swapchain image readback; CTest
runs that probe and treats missing validation/headless WSI support as an environment skip;
the local headless environment reaches Vulkan device selection but skips when llvmpipe
rejects its headless swapchain allocation. It can optionally
read a PPM reference image as its fourth argument and write per-frame PPM captures to a
directory supplied as its fifth argument. This makes future OSG/Vulkan captures
diagnosable without adding an image-library dependency or repeatedly restarting a game.
When a presentation-capable run reaches renderer creation, the smoke test also requires
the Khronos validation layer to be active and fails on error-level validation messages;
headless local runs still skip before that gate. CI runs the presentation smoke directly,
so an unexpected skip is a failure rather than a green test result; CI also invokes the
headless lifecycle probe and permits only its documented capability skip (77).
CI also rejects OSG/NIF/shared-logging includes and namespaces in the renderer-neutral and Vulkan
source boundaries before checking the linked smoke binary, preventing static linking from hiding
a boundary regression.
A neutral terrain tile snapshot adapter also converts the legacy
OSG-array/OSG-image storage contract into vertices, layer metadata, and RGBA8 blendmaps;
opaque single-layer terrain retains an intentionally absent blendmap. Vulkan now consumes
both opaque tiles and ordered multi-layer/blendmap tiles: blendmaps use the neutral alpha
texture table, legacy-compatible UV transforms, and separate first-layer/equal-depth
terrain pipelines. Terrain normal maps now flow through the same neutral texture table and
G-buffer normal path; parallax offsetting now uses the normal-map height channel. Scene
textures use a separate linear-repeat sampler so terrain tiling is preserved, while G-buffer
attachments remain clamped. Specular
maps and complete terrain image coverage remain outstanding. The neutral cache now retains
per-cell LOD snapshots and selects one deterministically by camera distance before handoff;
quadtree-scale streaming and composite-image coverage remain outstanding. `WorldScene` now
records empty loaded cells as well as object-bearing cells and owns each cell's cached terrain
LOD snapshots. `RenderingManager::getNeutralScene()` collects those snapshots for loaded
exterior cells in the active worldspace, so terrain is part of the real full-game neutral
handoff rather than only a test fixture; conversion happens on cell add/remove rather than
on every frame export.

Against the current `origin/openmw-vulkan` base, the current checkpoint changes
53 files, deleting 547 lines and adding 4,819 lines (net `+4,272`). The larger Vulkan-only
cleanup was completed in the merged PRs #1–#5; this PR is currently a groundwork expansion,
not the speculative 10k-line reduction. Further deletion must wait for a live Vulkan
consumer to replace the remaining OSG-owned responsibilities.

The latest validation checkpoint also rejects non-finite scene matrices, transforms, vertex
attributes, and terrain coordinates at the renderer-neutral submission boundary, before
they reach Vulkan. This protects the backend from corrupted engine state without relying
on GPU validation diagnostics.
The latest reduction checkpoint also removed the remaining manager-only neutral-object
lookup, removal, and cell-transfer wrappers. The lifecycle now calls `WorldScene`
directly. The terrain adapter is now consumed for opaque, normal-mapped, parallax, and
blendmap/multi-layer Vulkan terrain; its remaining owner boundary is quadtree-scale
streaming and complete image coverage. Terrain layer feature flags now default to disabled
at the shared storage boundary, preventing ESM4 default layers from acquiring undefined
parallax or specular state.
The Vulkan renderer now accepts only the aggregate `SceneSubmission`; its duplicate resolver
argument and local resolver type alias were removed.
Physical-device selection now checks the fixed G-buffer texture-array descriptor budget before
creating a logical device, so unsupported descriptor limits fail early instead of producing a
late descriptor-allocation or pipeline-validation error.
Vulkan buffer staging now reuses the device-owned memory-type lookup; the renderer no longer
duplicates that allocation policy in a local helper.
The descriptor pool now reserves only the scene/composite descriptors actually allocated;
the removed storage-image and excess-set capacity is gone.
Texture descriptor writes are also deferred until the owning frame fence has completed,
so scene updates do not mutate descriptor sets used by another in-flight frame.
Window and headless drawable-size handling now share one renderer helper, so acquire and
present recovery cannot drift between surface modes.
Swapchain creation now selects the first supported composite-alpha mode with opaque
composition preferred, and instance failures name the missing extension; unsupported
surface capabilities therefore fail at the boundary with actionable diagnostics.
Headless validation now enables the debug-utils extension as well, so a validation
layer that is available on a headless CI runner reports through the same error counter
as the windowed probe.
Instance creation now negotiates the loader's supported Vulkan version instead of requiring
1.3, and device creation no longer enables the unused anisotropy feature.
Resize, capture, and cleanup now retain only the synchronization waits required by
their ownership boundaries; redundant device/queue-idle calls were removed.
The swapchain no longer allocates a second unused depth image: depth is owned solely by
the renderer's G-buffer, removing the duplicate swapchain resource lifecycle.
The Vulkan device and swapchain are also now explicitly non-movable because both are
owned through `std::unique_ptr`; their unreachable custom move implementations are gone.

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
| Terrain geometry and layer data | Legacy OSG terrain storage/ChunkManager plus a tested neutral tile adapter, per-cell LOD selector, and Vulkan opaque/normal/parallax/blendmap/specular layer consumer | Add quadtree-scale terrain streaming and terrain image coverage |
| Loaded-cell object identity, transforms, terrain snapshots, and paging state | Renderer-neutral `WorldScene`/`CellScene` snapshots updated by scene lifecycle; cell-lifecycle-cached terrain tiles flow into `SceneSubmission` | Consume snapshots from a backend and migrate visibility/paging policy |
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

- Add a small test mode or executable that starts one renderer, loads a manifest of test scenes/cameras, renders multiple checkpoints, writes images, and exits. The current renderer-mesh CPU test validates NIF conversion, cache, and material setup; the standalone smoke target submits neutral mesh/terrain data, then covers one textured alpha-blended scene, reads back each rendered swapchain frame, compares consecutive captures when a Vulkan surface is available, and supports optional PPM reference/capture paths.
- Use fixed camera paths, time, weather, random seed, resolution, and content.
- Add CPU-side tests for matrix conversion, NIF conversion, transforms, resource lookup, and scene snapshots. The current fast tests cover matrix conversion, NIF conversion, parent-child transforms, safe index handling, cache reuse, cell-object transform composition, and renderer-neutral batch layout.
- Compare Vulkan output with OSG reference images using the neutral image comparator's
  tolerances and error metrics rather than exact pixel equality. Vulkan-to-Vulkan
  capture comparison is now wired into smoke; OSG reference-image execution remains
  pending until a presentation-capable validation host and reference capture workflow
  are available.

### 3. Remove the dual-renderer lifecycle

- Keep `OPENMW_USE_VULKAN` as the build gate for the standalone migration renderer until a
  complete full-game backend boundary exists; only then turn it into a compile-time game
  backend choice.
- Keep Vulkan translation units in the standalone `openmw_vulkan` library so the OSG game
  target does not link an inactive second renderer while the migration is isolated.
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

- Separate cell visibility, transforms, camera state, lighting, and material data from OSG scene nodes. Camera/light scene data now has an OSG-to-neutral snapshot source, alongside transform-preserving neutral mesh instances, updateable `WorldScene`/`CellScene` snapshots, explicit paged-reference visibility, neutral NIF material extraction, a cached-mesh cell composition adapter, loaded exterior terrain tiles, a concrete `SceneSubmission` handoff, and standalone Vulkan texture/alpha consumption; the live full-game backend call site and complete shading remain to be migrated.
- Feed both reference and Vulkan implementations from renderer-neutral scene data during the transition.
- Delete OSG scene ownership once Vulkan consumes all required scene events.

### 6. Port static world rendering

- Wire NIF loading and `MeshConverter` into resource management. The NIF converter now has a tested tree traversal and material boundary, `NifMeshManager` caches converted instances, image resources expose neutral RGBA8 data, and `RenderingManager` can collect a `SceneSubmission` containing static meshes and loaded exterior terrain without exposing OSG objects. The standalone Vulkan path consumes that submission and its resolved textures; a live full-game Vulkan frame consumer, shading, and complete static-world coverage are still outstanding.
- Implement model caching, cell add/remove, transforms, textures, materials, terrain,
  interiors, and static objects. The terrain adapter now feeds opaque and ordered
  blendmap/multi-layer Vulkan mesh consumers with normal-map sampling and height-based
  parallax and diffuse-alpha specular data; quadtree-scale terrain streaming and full terrain
  image coverage remain.
- Reach a static playable scene without OSG rendering.

### 7. Port dynamic content and presentation

- Add actors, skinning, animation, particles, weather, water, spell effects, and post-processing.
- Port GUI, fonts, loading screens, cursor handling, screenshots, and video presentation.
- Reintroduce ray tracing only after TLAS creation and the ray-tracing pipeline are complete; do not carry an inactive RT scaffold in the raster path.

### 8. Compare and delete

- Compare correctness, visual output, startup time, frame time, memory use, and mod compatibility.
- Delete duplicate adapters, dead OSG paths, obsolete Vulkan stubs, and transitional interfaces.
- Update the deletion ledger and line-count report after every subsystem removal.

### 9. Final OSG policy

The policy for this experiment is decided: keep OSG as a separate compatibility and
visual-reference build, but never initialize OSG and Vulkan in the same runtime. The
`openmw-vulkan` branch may continue to build the OSG reference configuration so visual
captures, startup behavior, and fallback compatibility remain available; that build is
not a second renderer in the Vulkan process. Vulkan becomes the active game backend only
after static and dynamic content, terrain, GUI, loading screens, screenshots, presentation,
and CI coverage reach the required parity gates. OSG dependencies may then be removed from
the active Vulkan build, while the reference build can remain separately if it still
provides maintenance or compatibility value.

## Test cadence

Normal development should use this order:

1. Compile and static checks.
2. CPU/unit tests.
3. One deterministic renderer-test process covering many scenes and checkpoints.
4. One validation-layer run.
5. A small number of full-game startup, save/load, GUI, and gameplay smoke tests.

The full game should not be repeatedly started for every change.
