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
Renderer-neutral mesh vertex normalization, index conversion, triangle-strip topology, and tangent generation now live in a separate
`openmw_render_neutral` library, which is consumed by both the legacy NIF adapter and
Vulkan-side tests without pulling the OSG-heavy `components` archive into the Vulkan
path. The focused neutral test covers index rejection and tangent-frame generation;
NIF parsing, material extraction, and scene-graph RTTI remain in the legacy adapter,
which now only translates source arrays into the neutral vertex-source contract.
The NIF mesh cache header depends on the NIF file and neutral mesh contracts directly;
the converter API is now an implementation dependency of the cache consumer only.
Terrain preload requests now use a renderer-neutral position and integer cell bounds;
the legacy terrain adapter performs the only conversion back to OSG vectors at the worker boundary.
The top-level build now also rejects a future `openmw-lib -> openmw_vulkan` link, and CI
checks the final ELF dependencies and renderer-symbol set in both binaries, making that
separation a configure- and link-time invariant. This keeps the process lifecycle
single-backend while the scene bridge is incomplete. The standalone Vulkan smoke target
now links only SDL2 and Vulkan at runtime; no OSG library or renderer symbol is present.
Vulkan configuration also probes `glslangValidator --version` and rejects a missing or
no-op shader compiler, so a successful build cannot silently omit the SPIR-V artifacts.
Its fixture now builds a renderer-neutral `WorldScene`, collects a `SceneSubmission`, and
feeds that aggregate to Vulkan, while the separate
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
visibility, cell transfer, and removal independently of the OSG node tree. Legacy-paged
references remain in the snapshot, but active-cell static references are submitted to the neutral
backend without inheriting OSG paging visibility. OSG still consumes the same events, but it no
longer needs to be the only source of object transform state. The neutral world path resolves
loaded-cell snapshots through cached NIF meshes, filters hidden and dynamic objects from the
static batch, and composes object transforms with NIF node transforms before batching;
animated objects are explicitly retained as dynamic snapshots, but are excluded from the static
mesh batch until a skinning/animation consumer owns them; the ordered `dynamicMeshes` payload provides
that future backend with the retained visibility, transform, model, and cell ordering together with
any resolved mesh data. A transitional OSG-backed producer now attaches current neutral bone matrices
after reference traversal, and the Vulkan consumer applies supplied poses through the
renderer-neutral CPU skinning helper before rasterization. Full-game Vulkan pose timing and
animation-specific shading remain outstanding, so records without a supplied pose are still
deliberately excluded rather than silently rendered in a bind pose.
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
manager. `MWWorld::Scene` now owns the neutral `WorldScene`; the world lifecycle writes object
snapshots during insertion and unpaging, while explicit neutral transform-update methods own
position, rotation, and scale changes. The manager retains OSG-facing object operations and
neutral light/fog updates; neutral terrain snapshot production now belongs to the storage
contract consumed by `Scene`. `Scene` also exposes the complete
neutral submission as the future full-game backend call site. World reset is
also owned by `Scene::clear()` after cell teardown. Terrain tile snapshots are now requested by
the scene lifecycle after OSG terrain setup and written directly to `WorldScene`. The manager also exposes a
renderer-neutral camera/inverse-matrix and directional-light snapshot
for a future Vulkan frame consumer. Neutral loaded-cell snapshots retain insertion order when
collected, making backend draw lists stable for image comparison and predictable alpha ordering.
Fog color and start/end distances now cross the same neutral scene handoff, and the Vulkan
composite applies the active linear fog range after reconstructing world position.
The CPU ABI guard tracks the expanded 336-byte scene UBO so future neutral-state additions
cannot silently desynchronize the Vulkan shader layout.
The neutral world snapshot now has an explicit reset path owned by `Scene::clear()` after cell
teardown, so a game/world unload cannot retain stale object identities, terrain tiles, or cell
ordering.
Dynamic `WorldObject` records now own an optional neutral bone-pose snapshot and expose an explicit
pose-update operation; `SceneSubmission` carries that pose beside each resolved dynamic mesh. The
neutral export now has a transitional OSG-backed pose producer: after the reference traversal,
it maps named NIF skin bones to current animation skeleton matrices and carries matching poses into
dynamic submissions. The full-game Vulkan frame loop still lacks this producer, animation timing
ownership, and animation-specific shading; mismatched multi-part skin orders are intentionally left
unposed. NIF skinning metadata now also preserves source bone names beside inverse-bind matrices,
making the future Vulkan-side mapping deterministic without borrowing OSG types.
The Vulkan composite pass now consumes that single scene-lighting UBO directly; duplicated
sun push constants were removed, and ambient light is part of the neutral snapshot. The
world-owned `Scene` now resolves loaded-world meshes and RGBA8 textures through the existing
resource caches, giving a future Vulkan consumer a concrete full-game input without exposing
OSG objects or restoring manager-owned scene assembly. Its neutral mesh lookup cache keeps weak
references across frame exports, avoiding repeated conversion lookups without extending resource
lifetimes beyond the resource manager.
The neutral camera snapshot now reads the cached camera matrices instead of querying the OSG
viewer directly, and neutral lighting/fog values are updated at their game-state setters rather
than re-read from OSG objects during export. `Camera` now exposes neutral `Render::Mat4` snapshots
for this path while retaining legacy OSG getters for the reference backend. This removes another
backend-specific type from the future Vulkan handoff.
Those inputs can now be collected as one `Render::SceneSubmission`; the standalone smoke
consumer now exercises the same `WorldScene` to `Vk::Renderer` handoff, while the renderer-neutral
`collectSceneSubmission` helper owns mesh, dynamic-record, worldspace, and terrain selection.
`Vk::Renderer::setScene` supports persistent frame-loop updates, and
`Vk::Renderer::render(SceneSubmission)` provides the atomic one-call handoff for a future live
backend. The standalone `Vk::Renderer` also implements `Render::FrameLifecycle`, so the smoke
test exercises the same submission-consuming frame-owner contract used by the future game path.
The submission
boundary now validates mesh indices and terrain snapshots before Vulkan consumes them. The full-game
Vulkan call site is still intentionally absent until window, input, dynamic-content, and GUI
services have a Vulkan owner. NIF skinning metadata now survives conversion, and resolved dynamic
mesh payloads cross the neutral boundary into the Vulkan consumer. A deterministic CPU skinning
helper now applies frame bone matrices for future animation integration. Unskinned dynamic meshes and skinned records with a
supplied pose now enter the raster draw batch with their neutral transforms; skinned records without a pose remain outside it
until per-frame bone updates are owned. Mesh submission no longer waits for the whole device or
rebuilds one global buffer: neutral mesh data is retained on the CPU and uploaded into
the current frame slot only after its fence is waited, so a future live frame loop can
submit scene updates without the previous device-wide stall.
An opt-in `OPENMW_VALIDATE_NEUTRAL_SCENE=1` full-game run validates the submission and
resolves every referenced mesh/terrain texture periodically during one world session,
providing a bridge check without initializing a second renderer.
Neutral cells now retain worldspace identity, and both mesh and terrain collection filter
to the active worldspace so an unloaded or inactive worldspace cannot leak into a Vulkan
submission.
The fast test suite now also contains a backend-neutral RGBA8 image comparator with
per-channel tolerance, differing-pixel count, maximum error, and mean error metrics.
The Vulkan smoke path now reads back rendered RGBA8/BGRA8 swapchain frames and compares
consecutive captures with that comparator when a presentation-capable host is available;
without a reference image it also runs three neutral scene checkpoints in the same process:
initial static/dynamic/terrain content, a material/light replacement, and object/terrain
removal. This exercises descriptor growth, frame-safe mesh replacement, and scene ownership
updates without repeatedly restarting a game;
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
attachments remain clamped. Terrain diffuse-specular maps are consumed, and ordinary object
specular maps now use the configured pattern, dedicated texture table, and RGB G-buffer path;
ESM4 explicit and auto-detected terrain specular maps now use the same neutral texture table and RGB path;
complete terrain image coverage remains outstanding. The neutral cache now retains
per-cell LOD snapshots and also assembles aligned multi-cell region snapshots, selecting one
region LOD deterministically by camera distance before handoff; adjacent regions are constrained
to at most a one-level LOD gap to reduce cracks. Incomplete regions fall back to the per-cell
path. Geometry neighbor stitching, quadtree streaming policy, and composite-image coverage
remain outstanding. `WorldScene` now
records empty loaded cells as well as object-bearing cells and owns each cell's cached terrain
LOD snapshots. `MWWorld::Scene::getNeutralScene()` now assembles those snapshots for loaded
exterior cells in the active worldspace, so terrain is part of the real full-game neutral
handoff rather than only a test fixture; conversion happens on cell add/remove rather than
on every frame export. Neutral terrain LOD assembly now belongs to `Terrain::RenderStorage`, while
`RenderingManager` only provides that storage to the world lifecycle; it no longer assembles
the renderer-neutral scene submission or owns its resource callbacks. Neutral terrain collection
now depends on cached world tiles rather than the legacy OSG terrain object being active.
`Terrain::RenderStorage` is now the renderer-neutral terrain contract, including neutral
per-cell LOD assembly. `MWWorld::Scene` receives that contract explicitly at construction,
so its neutral terrain paths do not call back into `RenderingManager`. The legacy
`Terrain::Storage` derives from it and contains only the OSG-facing array, image, and height
adapters needed by the reference renderer. `RenderStorage::getRenderTile()` consumes neutral
vertices and blendmaps directly, so the Vulkan path no longer performs an OSG-buffer-to-neutral
round trip and can depend on the contract without including OSG headers. `World` now owns the
concrete `MWRender::TerrainStorage` lifetime and passes it explicitly to the OSG manager and
world scene; `RenderingManager` consumes it as a reference for legacy terrain setup instead of
owning a second terrain-storage lifetime.
Neutral lighting and fog state now lives in `WorldScene::sceneData()` with the rest of the
world-owned handoff. `RenderingManager` exposes an explicit frame-boundary synchronization
operation, so its OSG state changes cannot reintroduce a second neutral scene owner or retain
a pointer into the world scene.
The neutral terrain tile entry point also uses a plain two-float center; OSG vector types
remain confined to the legacy quadtree and reference-renderer methods.
Camera and environment state are synchronized into that same world-owned state at the renderer
frame boundary; neutral submission export uses the same explicit synchronization operation for
a submission-consuming backend.
Neutral object and groundcover snapshots now compose Euler and axis-angle rotations through
renderer-neutral math; OSG quaternion construction remains only for legacy scene-node updates.
The full-game bridge validator runs after that same render boundary, so it validates the
camera payload that was just submitted rather than the previous frame's cached matrices.
The non-owning manager update handle is private to the `Scene` owner, detached during `Scene`
teardown, and CI guards the manager header against regaining a value-owned neutral frame state.
Engine GUI fallback frame advancement now also reads simulation time from the active
`FrameLifecycle`, keeping renderer orchestration from reaching directly into an OSG frame stamp.
The same interface now owns the engine-visible frame number: the OSG adapter reads its frame stamp,
while the Vulkan owner advances its neutral counter with simulation-frame advancement.
Loop termination is also delegated through the lifecycle; the OSG adapter preserves viewer shutdown
semantics, while a Vulkan owner can use the engine quit-request path without an OSG viewer query.
The engine public header no longer imports complete OSG viewer/event-handler headers; OSG declarations
are now included only by the implementation files that use them.

Against the current `origin/openmw-vulkan` base, the current checkpoint changes
94 files, deleting 928 lines and adding 7,359 lines (net `+6,431`). The larger Vulkan-only
cleanup was completed in the merged PRs #1–#5; this PR is currently a groundwork expansion,
not the speculative 10k-line reduction. Further deletion must wait for a live Vulkan
consumer to replace the remaining OSG-owned responsibilities.

Submission validation now lives on the renderer-neutral `SceneSubmission` boundary: the
full-game bridge and Vulkan consumer use the same geometry, resolver, and texture-resource
gate, including dynamic mesh textures. The latest validation checkpoint also rejects non-finite scene matrices, transforms, vertex
attributes, skinning payloads, and terrain coordinates at the renderer-neutral submission boundary, before
they reach Vulkan. This protects the backend from corrupted engine state without relying
on GPU validation diagnostics. Visible models that resolve only to empty mesh batches are also
reported as unresolved, so a converter cannot silently turn a world reference into no draw.
Embedded alpha textures are validated at the same boundary, preventing invalid transparency data
from being replaced by a backend fallback. `World` now synchronizes and assembles the submission,
while the consuming frame owner performs the single resolver validation before backend upload;
the previous world-side duplicate validation has been removed.
Neutral scene data now starts with identity camera and inverse matrices plus explicit safe
lighting/fog defaults, so a world reset or pre-camera frame cannot silently submit an all-zero
camera state. The world-scene CPU test covers both initial construction and reset.
The latest reduction checkpoint also removed `RenderingManager`'s neutral `WorldScene` ownership,
the public neutral-world lookup escape hatch, and its remaining manager-only neutral-object
lookup, removal, cell-transfer, transform-update, reset, and terrain-snapshot wrappers. The
world lifecycle now calls `WorldScene` directly for
insertion, removal, active-cell transfer, transform updates, reset, and terrain snapshots. Active-cell static references are now submitted even when legacy OSG object paging places them in a paged node; the neutral backend therefore does not inherit an invisible-object hole from the reference renderer. The terrain adapter is now consumed for opaque, normal-mapped, parallax, and
blendmap/multi-layer Vulkan terrain, including explicit ESM4 and auto-detected specular textures; its remaining
owner boundary is quadtree-scale streaming and complete image coverage. Terrain layer feature
flags now default to disabled
at the shared storage boundary, preventing ESM4 default layers from acquiring undefined
parallax or specular state.
Neutral scene export receives the world-owned `ResourceSystem` directly for mesh and texture
resolution, so `RenderingManager` is not used as that renderer-neutral resource gateway.
The manager's OSG-owned terrain, object paging, incremental compile operation, light root, sky,
and postprocessor are now passed explicitly during world construction; these are no longer public
manager service gateways.
Scene/resource cache timing now comes from the renderer-neutral `FrameLifecycle` clock, with OSG
and Vulkan owners supplying their own reference time.
Neutral mesh and texture resolution, including optional specular-file discovery, now crosses the
scene boundary as injected callbacks rather than direct `ResourceSystem` calls.
Neutral scene synchronization and bone-pose production now cross the same boundary as injected
callbacks; the current providers remain OSG-backed until a Vulkan game owner replaces them.
RGBA8 conversion is now one renderer-neutral helper shared by image resources and terrain
blendmaps, so clamping, finite-value rejection, and byte quantization cannot drift between
resource paths. The conversion helper has direct CPU coverage.
Neutral image resolution also rejects the legacy warning-image fallback, so the opt-in
full-game bridge validator cannot report missing or unsupported textures as successful
RGBA8 resources.
The scene collector also carries visible models that resolve to no converted geometry as
explicit unresolved entries; submission validation rejects those entries instead of silently
dropping world references.
Groundcover records are now density-filtered at the world adapter, converted into neutral
cell-owned static instances, and sent through the same mesh/material resolver as ordinary
static references; the standalone Vulkan consumer therefore has a neutral groundcover input
without depending on the OSG chunk manager.
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
| NIF-to-neutral mesh conversion | Renderer-neutral NIF boundary, material data, mesh cache, skinning metadata, dynamic mesh payloads, `SceneSubmission`, Vulkan mesh batch, standalone texture table, and full-game neutral resolver | Connect the handoff to the live full-game Vulkan frame loop, add per-frame bone updates and dynamic shading |
| Terrain geometry and layer data | Renderer-neutral `Terrain::RenderStorage` contract with cached per-cell LOD snapshots and a Vulkan opaque/normal/parallax/blendmap/specular layer consumer; concrete `MWRender::TerrainStorage` and legacy OSG ChunkManager remain the reference data path, including explicit ESM4 specular textures | Add quadtree-scale terrain streaming and terrain image coverage |
| Loaded-cell object identity, transforms, terrain snapshots, and paging state | Renderer-neutral `WorldScene`/`CellScene` snapshots updated by scene lifecycle; active-cell static references bypass legacy OSG paging visibility, and cell-lifecycle-cached terrain tiles flow into `SceneSubmission` | Consume snapshots from a backend and migrate visibility/paging policy |
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

- Add a small test mode or executable that starts one renderer, loads a manifest of test scenes/cameras, renders multiple checkpoints, writes images, and exits. The current renderer-mesh CPU test validates NIF conversion, cache, and material setup; the standalone smoke target submits neutral mesh/terrain data, then covers three textured scene-ownership checkpoints in one process, reads back each rendered swapchain frame, compares consecutive captures when a Vulkan surface is available, and supports optional PPM reference/capture paths.
- Use fixed camera paths, time, weather, random seed, resolution, and content.
- Add CPU-side tests for matrix conversion, NIF conversion, transforms, resource lookup, and scene snapshots. The current fast tests cover matrix conversion, NIF conversion, parent-child transforms, safe index handling, cache reuse, cell-object transform composition, renderer-neutral batch layout, and static mesh/terrain removal while dynamic ownership remains.
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

- Separate cell visibility, transforms, camera state, lighting, and material data from OSG scene nodes. Camera/light scene data now has an OSG-to-neutral snapshot source, alongside transform-preserving neutral mesh instances, updateable `WorldScene`/`CellScene` snapshots, neutral visibility for active-cell static references, neutral NIF material extraction, a cached-mesh cell composition adapter, loaded exterior terrain tiles, a pure `collectSceneSubmission` handoff exposed by `MWWorld::Scene`, and standalone Vulkan texture/alpha consumption; the live full-game backend call site and complete shading remain to be migrated.
- Feed both reference and Vulkan implementations from renderer-neutral scene data during the transition.
- Delete OSG scene ownership once Vulkan consumes all required scene events.

### 6. Port static world rendering

- Wire NIF loading and `MeshConverter` into resource management. The NIF converter now has a tested tree traversal and material boundary, `NifMeshManager` caches converted instances, image resources expose neutral RGBA8 data, and `RenderingManager` can collect a `SceneSubmission` containing static meshes and loaded exterior terrain without exposing OSG objects. The standalone Vulkan path consumes that submission and its resolved textures; a live full-game Vulkan frame consumer, shading, and complete static-world coverage are still outstanding.
- Implement model caching, cell add/remove, transforms, textures, materials, terrain,
  interiors, and static objects. The terrain adapter now feeds opaque and ordered
  blendmap/multi-layer Vulkan mesh consumers with normal-map sampling, height-based
  parallax, diffuse-alpha specular data, and explicit ESM4 specular textures; quadtree-scale
  terrain streaming and full terrain image coverage remain.
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

When scene-bridge changes are in progress, enable `OPENMW_VALIDATE_NEUTRAL_SCENE=1` for
the full-game smoke run; it checks the neutral payload every 30 frames and after cell changes.

The full game should not be repeatedly started for every change. The main engine loop, loading
screen, modal/video loops, and screenshot capture now delegate frame advancement, event processing,
update traversal, and frame submission through one engine-owned frame lifecycle. The engine creates
one OSG `ViewerFrameLifecycle` before world initialization and passes that service explicitly to
`World` and `RenderingManager`; no second OSG lifecycle adapter is constructed by the manager.
Direct OSG frame operations remain only in that adapter, while bootstrap callbacks use the same small
frame-owner type that can be replaced with the Vulkan presentation owner. This establishes the
replacement point for a future Vulkan frame owner while current OSG behavior remains unchanged.
The interface now has an explicit submission-consuming path: a Vulkan owner
will receive a synchronized `SceneSubmission` from `World` and validate it before upload, while the OSG owner
explicitly reports that it does not consume submissions and continues its legacy traversal.
The engine-side periodic bridge validator skips its duplicate export when that path is active,
leaving submission validation at the single world/frame-owner boundary.
