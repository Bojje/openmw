# Vulkan renderer migration plan

This plan deliberately prioritizes removal of duplicate renderer code. OpenSceneGraph (OSG) remains available only as a reference implementation while Vulkan is being brought to parity; it is not kept alive through a permanent compatibility layer.

## Working rules

1. Preserve the current OSG implementation in Git (a tag or reference worktree), not in the active Vulkan execution path.
2. Every new abstraction must replace an existing responsibility or have a concrete deletion milestone.
3. Vulkan and OSG must never both initialize or render in one process.
4. A deletion checkpoint must compile, pass fast tests, and pass the deterministic renderer smoke test before the next subsystem is removed.
5. Visual parity is the minimum bar; performance and visual improvements come after parity.

## Current checkpoint

The experiment is currently isolated on the `openmw-vulkan` branch. The default game remains
OSG-backed, while `OPENMW_USE_VULKAN=ON` now builds the Vulkan game backend and migration
tests; `openmw --vulkan` selects one Vulkan frame owner at startup. The experimental game
path creates a Vulkan SDL window, neutral resource services, a neutral world scene, and a
single Vulkan submission consumer. It is intentionally no-GUI and rejects unsupported
textures at the neutral submission boundary while neutral image coverage is expanded. Vulkan translation units now
live in a separate `openmw_vulkan` library instead of the shared `components` archive.
Renderer-neutral mesh vertex normalization, index conversion, triangle-strip topology, and tangent generation now live in a separate
`openmw_render_neutral` library, which is consumed by both the legacy NIF adapter and
Vulkan-side tests without pulling the OSG-heavy `components` archive into the Vulkan
path. The focused neutral test covers index rejection and tangent-frame generation;
NIF parsing, material extraction, and scene-graph RTTI remain in the legacy adapter,
which now only translates source arrays into the neutral vertex-source contract.
The NIF mesh cache header depends on the NIF file and neutral mesh contracts directly;
the converter API is now an implementation dependency of the cache consumer only.
The legacy image manager no longer exports an unused `osg::Texture2D` include; texture construction
includes are now owned by the OSG call sites that actually construct them.
Terrain preload requests now use a renderer-neutral position and integer cell bounds;
the legacy terrain adapter performs the only conversion back to OSG vectors at the worker boundary.
Scene cell-grid policy now keeps its center and bounds as plain integer arrays as well;
OSG grid values are created only at the legacy renderer and navigation call sites.
Neutral object rotation updates now recompute the direct ESM rotation instead of copying
the OSG node’s inverse-order quaternion, keeping backend snapshots stable across player and script rotations.
The unused OpenMW-level neutral pose mutator was removed; dynamic poses now have one live producer
at submission time while the renderer-neutral `WorldScene` pose API remains independently testable.
The engine now constructs exactly one renderer-specific frame lifecycle: OSG by default or
Vulkan when explicitly selected. OSG and Vulkan are not initialized in the same runtime;
OSG libraries still remain in the transitional full-game link because GUI, legacy world
services, and shared engine code have not yet been deleted. CI must therefore distinguish
runtime ownership from the temporary link footprint. The standalone Vulkan smoke target
continues to link only SDL2 and Vulkan at runtime; no OSG library or renderer symbol is present.
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

Completed reduction checkpoints include removal of the incomplete standalone Vulkan bridge,
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
backend without inheriting OSG paging visibility. The OSG path remains legacy-owned; the neutral world path resolves
loaded-cell snapshots through cached NIF meshes, filters hidden and dynamic objects from the
static batch, and composes object transforms with NIF node transforms before batching;
animated objects are explicitly retained as dynamic snapshots, but are excluded from the static
mesh batch until a skinning/animation consumer owns them; the ordered `dynamicMeshes` payload provides
that future backend with the retained visibility, transform, model, and cell ordering together with
any resolved mesh data. The neutral Vulkan bootstrap still has no per-frame animation producer, but
skinned records now receive an inverse-bind-derived bind pose so they remain visible while the
animation owner is ported. The renderer-neutral CPU skinning helper and converted skinning metadata
remain ready for that later animation owner.
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
The neutral resource backend now owns a VFS-backed RGBA8 texture cache with TGA, BMP, PNG, JPEG, and
common DDS/DXT decoding; the live Vulkan bootstrap uses it for real static-world texture
paths; unsupported formats now fail the submission with their exact resource path instead of
silently becoming white.
Classic and BS shader texture wrap flags now select per-resource repeat/clamp sampler variants.
The standalone Vulkan renderer uploads/caches indexed albedo textures and samples them
in the G-buffer (currently bounded to a 64-entry table). Vulkan now consumes neutral
alpha-test state and thresholds in the G-buffer
cutout path, and the standalone harness has a basic source-alpha pipeline for blended draws;
ordinary alpha draws are now sorted back-to-front with stable ties, while ordered terrain
layers retain their submission order; full material shading and complete image-format coverage
remain outstanding.
The Vulkan G-buffer now carries neutral roughness,
ambient-occlusion,
and emissive-strength channels into the composite pass. Cell object lookup and removal are
owned by the renderer-neutral `WorldScene`/`CellScene` components rather than the OSG-facing
manager. `MWWorld::Scene` now owns the neutral `WorldScene`; the world lifecycle writes object
snapshots during insertion and unpaging, while explicit neutral transform-update methods own
position, rotation, and scale changes. The manager retains OSG-facing object operations and
neutral light/fog updates; object-class insertion now receives `MWRender::Objects` directly
instead of a virtual manager adapter; neutral terrain snapshot production now belongs to the storage
contract consumed by `Scene`. `Scene` also exposes the complete
neutral submission as the future full-game backend call site. World reset is
also owned by `Scene::clear()` after cell teardown. Terrain tile snapshots are now requested by
the scene lifecycle after OSG terrain setup and written directly to `WorldScene`. The manager also exposes a
renderer-neutral camera/inverse-matrix and directional-light snapshot
for a future Vulkan frame consumer. Neutral loaded-cell snapshots retain insertion order when
collected, making backend draw lists stable for image comparison and predictable alpha ordering.
Fog color and start/end distances now cross the same neutral scene handoff, and the Vulkan
composite applies the active linear fog range after reconstructing world position.
The CPU ABI guard tracks the expanded 352-byte scene UBO so future neutral-state additions
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
The OSG reference adapter exposes neutral camera matrices without requiring the Vulkan consumer to
query the OSG viewer. The full-game Vulkan owner now supplies its own renderer-neutral camera
snapshot from player state, including full orientation and drawable-size projection, while the
legacy `Camera` continues to retain its OSG getters for the reference backend.
The no-GUI Vulkan bootstrap now supplies a renderer-neutral first-person look-at and perspective
snapshot from the player transform, including full pitch/roll orientation and resize-aware
projection updates; camera-controller parity remains outstanding.
Player update logic now treats the absent OSG presentation owner as an explicit Vulkan mode, and
player-only OSG/MyGUI Lua packages, including menu/UI packages, are withheld from that runtime
instead of exposing null renderer dereferences. Lua lifecycle cleanup and input filtering also
use the neutral window manager and skip absent post-processing services.
Those inputs can now be collected as one `Render::SceneSubmission`; the standalone smoke
consumer now exercises the same `WorldScene` to `Vk::Renderer` handoff, while the renderer-neutral
`collectSceneSubmission` helper owns mesh, dynamic-record, worldspace, and terrain selection.
`Vk::Renderer::setScene` supports persistent frame-loop updates, and
`Vk::Renderer::render(SceneSubmission)` provides the atomic one-call handoff for a future live
backend. The standalone `Vk::Renderer` also implements `Render::FrameLifecycle`, so the smoke
test exercises the same submission-consuming frame-owner contract used by the future game path.
The submission
boundary now validates mesh indices and terrain snapshots before Vulkan consumes them. The
full-game Vulkan call site now exists for the no-GUI bootstrap: window/input ownership,
camera synchronization, neutral image loading, and static scene submission are live, while
dynamic-content, weather particle/water presentation, and GUI/presentation services remain incomplete. Weather ambient, directional-light, fog, and sky-horizon values now cross the neutral frame state, and the Vulkan composite derives its background gradient from that state. NIF skinning metadata now survives conversion, and resolved dynamic
mesh payloads cross the neutral boundary into the Vulkan consumer. A deterministic CPU skinning
helper now applies frame bone matrices for future animation integration. Unskinned dynamic meshes and skinned records with a
supplied pose now enter the raster draw batch with their neutral transforms; skinned records without a resolved pose now
use an inverse-bind-derived bind pose so dynamic actors remain visible while per-frame animation updates are ported.
Mesh submission no longer waits for the whole device or
rebuilds one global buffer: neutral mesh data is retained on the CPU and uploaded into
the current frame slot only after its fence is waited, so a future live frame loop can
submit scene updates without the previous device-wide stall.
Neutral cells now retain worldspace identity, and both mesh and terrain collection filter
to the active worldspace so an unloaded or inactive worldspace cannot leak into a Vulkan
submission.
The neutral game camera now retains POV state and produces a bounded third-person orbit behind
the player when POV is toggled, instead of silently remaining first-person without an OSG camera.
Neutral vanity mode now also owns its orbit yaw/pitch, restores the prior POV when disabled, and
loads/saves the existing `FIRS` camera state without constructing an OSG camera.
The fast test suite now also contains a backend-neutral RGBA8 image comparator with
per-channel tolerance, differing-pixel count, maximum error, and mean error metrics.
The renderer test family also includes a runnable resource-backend check: neutral resource
initialization retains shared image/mesh services but does not construct OSG scene or keyframe
services. This regression does not depend on the optional GoogleTest component-test download.
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
a boundary regression. It now also rejects a stored `ResourceSystem` gateway in `MWWorld::Scene`
and runs the neutral resource-backend regression in the Vulkan job.
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
`Scene::recordNeutralCell()` now owns the renderer-neutral cell identity, groundcover, and terrain
snapshot update as one explicit phase before legacy OSG insertion. This keeps neutral cell state
complete even as the later OSG side effects are replaced by a Vulkan backend.
Object snapshots follow the same ordering: active-cell neutral ownership is recorded before OSG
object insertion, and neutral visibility is not inherited from legacy paging.
`RenderingManager` only provides that storage to the world lifecycle; it no longer assembles
the renderer-neutral scene submission or owns its resource callbacks. Neutral terrain collection
now depends on cached world tiles rather than the legacy OSG terrain object being active.
`Terrain::RenderStorage` is now the renderer-neutral terrain contract, including neutral
per-cell LOD assembly. `MWWorld::Scene` receives that contract explicitly at construction,
so its neutral terrain paths do not call back into `RenderingManager`. The legacy
`Terrain::Storage` derives from it and contains only the OSG-facing array, image, and height
adapters needed by the reference renderer. Terrain collision samples now cross that same
contract as owned neutral heightfield data; Bullet retains the samples itself, so `Scene` no
longer reads an OSG `LandObject` or relies on its lifetime when creating collision and navigation
heightfields. `RenderStorage::getRenderTile()` consumes neutral
vertices and blendmaps directly, so the Vulkan path no longer performs an OSG-buffer-to-neutral
round trip and can depend on the contract without including OSG headers. `World` now owns the
concrete `MWRender::TerrainStorage` lifetime and passes it explicitly to the OSG manager and
world scene; `RenderingManager` consumes it as a reference for legacy terrain setup instead of
owning a second terrain-storage lifetime.
Neutral lighting and fog state now lives in `WorldScene::sceneData()` with the rest of the
world-owned handoff. `RenderingManager` exposes a value-returning frame-state snapshot, so its
OSG state changes cannot reintroduce a second neutral scene owner or retain a pointer into the
world scene.
The neutral terrain tile entry point also uses a plain two-float center; OSG vector types
remain confined to the legacy quadtree and reference-renderer methods.
Camera and environment state are copied into that same world-owned state at the renderer frame
boundary; neutral submission export uses the same explicit value snapshot for a submission-consuming
backend.
Neutral object and groundcover snapshots now compose Euler and axis-angle rotations through
renderer-neutral math; OSG quaternion construction remains only for legacy scene-node updates.
Object scale adjustment now also uses the renderer-neutral `Render::Vec3` contract; OSG scale
conversion is limited to the legacy rendering-manager and preview boundaries.
Scene cell-grid decisions, deferred grid changes, and terrain-preload prediction now retain
positions as `Render::Vec3`; conversion back to OSG is limited to legacy navigator, cell-loading,
and preloader calls. Cell loading also no longer carries an unused player-position argument,
removing a stale OSG conversion from every load path.
`FrameLifecycle` now identifies its backend explicitly. The current `World::init` path rejects
non-OSG owners before allocating OSG physics, terrain, or scene services; this is a deliberate
fail-fast guard against accidentally constructing Vulkan and OSG in one runtime, while the
backend-service replacement is still being implemented.
Physics simulation construction is now independent of an OSG parent node; collision debug
geometry is an explicit optional OSG setup step. A Vulkan owner can therefore construct the
simulation core without creating an OSG scene node, while the reference path retains collision
visualization unchanged.
`World` initialization now follows the same ownership split: `initSimulation()` creates
physics/navigation services, `initOsgRenderer()` creates only the OSG terrain, scene graph, and
legacy manager, and `initNeutralRenderer()` creates only the neutral scene and resource handoff.
The engine calls these phases explicitly, so the OSG path no longer maintains a duplicate
renderer-neutral scene for validation.
`ResourceSystem` now has an explicit OSG/neutral backend mode: neutral initialization constructs
only the shared NIF file/mesh services and omits `SceneManager`, `KeyframeManager`, image, material,
and animation-rule managers. Neutral scene services receive texture data through injected
renderer-neutral resolvers, while the world renderer rejects a mismatched resource backend before
touching presentation services. This keeps the future Vulkan path from paying for OSG scene and
image ownership merely because the shared resource facade still exists.
The simulation, OSG renderer, and neutral renderer bootstrap phases now all validate that resource
backend identity matches the selected frame owner, preventing a partial Vulkan path from silently
retaining OSG resource services.
The OSG renderer also validates that its light-root handoff was populated before constructing
the projectile presenter, turning a previously unchecked startup dereference into a fail-fast
diagnostic.
The non-owning manager update handle is private to the `Scene` owner, detached during `Scene`
teardown, and CI guards the manager header against regaining a value-owned neutral frame state.
`Scene` no longer retains pass-through ownership of the legacy land manager, and its temporary
cell-test suppression restores the scene manager's current incremental-compile operation locally.
The scene no longer stores that incremental-compile pointer or a second land-manager reference;
both are now scoped to the legacy operations that need them.
The `Scene` constructor no longer accepts the unused `LandManager` pass-through either; the
OSG world bootstrap gives that dependency directly to `CellPreloader`.
The scene constructor now accepts the legacy `RenderingManager` and `CellPreloader` as optional
services. Neutral cell/object/terrain state remains available when they are absent, while every
OSG paging, object, water, and preload call is guarded; the current game bootstrap still supplies
both services through the OSG-only initialization path.
Legacy mesh preloading now receives the OSG `SceneManager` directly, and `Scene` no longer retains
a `ResourceSystem` pointer. The neutral path still retains the shared VFS and NIF-backed resource
services needed for scene data, but cannot accidentally reach through the resource facade to
construct or query an OSG scene service; the legacy cell-test commands resolve that service only
inside their OSG-only operations.
The `SceneManager` is now passed only to the OSG preload operation itself; neutral `Scene`
construction no longer stores or accepts that optional service.
`CellPreloader` construction is likewise owned by the OSG world-renderer bootstrap and passed as
an already-created legacy service; `Scene` no longer accepts a `ResourceSystem` pointer at all.
The neutral scene also no longer caches OSG-only preloader settings; those values are read only by
the active legacy preloader operations, leaving the scene state smaller on the neutral path.
The fixed legacy cell-loading threshold is local to the OSG preload calculation as well, removing
another preloader-only field from `Scene`.
Standalone effect-mesh preloading now lives beside cell and terrain preloading in `CellPreloader`.
This removes the OSG work queue and asynchronous work-item ownership from `Scene`, and reduces the
public preload call to mesh data plus animation intent; neutral scene construction no longer carries
an OSG queue or scene-manager plumbing for a service it cannot execute.
The active paged-reference cache now also belongs to `ObjectPaging`; `Scene` forwards only the legacy
unpaging transition, while OSG intersection and cull queries ask the paging service directly. This
removes another OSG-owned vector and lookup helper from the neutral scene owner.
The duplicate `Terrain::World*` in `Scene` is removed as well: terrain-worldspace checks and
view rebuilding now stay inside the OSG-only `CellPreloader`, so neutral scene construction no
longer accepts or stores a legacy terrain-world pointer.
Object insertion now shares one world/physics/mechanics path for both backends: the optional OSG
adapter handles only scene-graph insertion and water-ripple registration, while neutral object
snapshots, physics shapes, looping effects, and lifecycle notifications are not skipped when OSG
is absent. The duplicated neutral/OSG insertion branches were removed.
Non-actor half-extents also use physics bounds when the OSG adapter is absent, so neutral gameplay
queries no longer require `RenderingManager` geometry traversal.
The world update boundary now skips OSG rendering, weather, sound-listener, and loading-screen
work when those optional services are absent; simulation, navigation, physics, and neutral scene
updates can therefore advance under a submission-consuming frame owner.
The remaining world-facing interaction helpers now follow that same rule: neutral startup does
not touch camera/UI/audio services, focus polling becomes a no-op until a presentation owner
exists, and rendering-ray queries fall back to the physics ray caster. Preview, screenshot,
settings, spell-feedback, damage-feedback, and jail presentation calls are likewise optional.
Player setup/rendering now follows the same split: OSG animation and node work is optional, while
physics actor creation, mechanics registration, inventory listener clearing, effects, navigation,
and neutral transform updates remain shared. This removes another first-frame null dereference in
the neutral bootstrap.
Object movement and active-cell transfer now publish neutral positions and update physics without
requiring an OSG base node; the legacy manager is limited to node movement, pointer transfer, and
paging blacklist updates.
Scripted world rotation now follows the same neutral transform path, and animation/head queries
return safe neutral fallbacks when no OSG presentation owner exists.
World clearing, time advancement, sky state, and cell transfer now guard optional weather, paging,
and sky services; those simulation state changes no longer require an OSG renderer instance.
`WeatherManager` now owns weather/time simulation independently of OSG; neutral bootstrap creates
it without sky/fog pointers, while the OSG adapter receives the same state for legacy presentation.
Water height remains owned by physics in neutral mode, and exterior cell bounds now cross the
neutral world snapshot as bounded `WaterSurface` records. The Vulkan submission path emits a simple
toggleable alpha-blended planar water consumer; reflections, refraction, ripples, interior-water
bounds, and the full legacy water shader remain outstanding. Malformed neutral water records are
rejected at the same submission validation boundary as invalid geometry.
`World` now retains terrain through `Terrain::RenderStorage`; the concrete OSG terrain adapter is
created only inside OSG initialization and retained polymorphically, removing that concrete type
from neutral world ownership.
Simulation initialization now passes the active frame backend into physics. Vulkan/neutral physics
does not acquire `SceneManager`; NIF collision shapes remain available through the neutral NIF loader,
while non-NIF scene-derived collision shapes are rejected explicitly until a neutral loader replaces
that OSG-only path.
World update no longer performs GUI-dependent spell preloading or jail-window checks without the
legacy renderer/UI owner.
Projectile bookkeeping is likewise optional at the neutral simulation boundary; physics can advance
and the world can clear and shut down without constructing the OSG projectile presenter. Unsupported
weather/projectile save records are explicitly skipped in neutral mode and are not claimed as
save-compatibility parity; projectile visuals and presentation-specific effects remain a later
dynamic-content milestone.
Cell-transition loading screens, window-manager cell notifications, actor watching, fades, and
postprocessor flags now follow the same legacy-service guard, so the neutral bootstrap does not
silently re-enter the OSG/UI path during cell changes.
Cell teardown also stops legacy audio only when the presentation owner exists; neutral cell
unload now remains simulation-only instead of reaching through the absent sound service.
`World::adjustSky()` also returns before touching the legacy renderer when no OSG services exist,
covering the first neutral cell transition without a hidden null dereference; CI checks this guard.
Terrain height queries now cross `Terrain::RenderStorage` as renderer-neutral world data; the
legacy `TerrainStorage` adapter performs the only conversion back to OSG coordinates. `World`
does not fall back to `RenderingManager` for terrain queries, so the neutral bootstrap has one
terrain-data owner and CI checks that boundary.
`World::initNeutralRenderer()` now exposes that boundary to a submission-consuming Vulkan owner:
it requires simulation first, rejects an OSG frame owner, and constructs the world scene without
allocating any OSG rendering, paging, terrain-world, or preloader service.
That bootstrap now constructs and owns `NeutralTerrainStorage` directly from `ESMStore` and the VFS;
the provider emits renderer-neutral TES3 vertices, normals, colors, heightfields, bounds, and
blendmaps without `ESMTerrain::Storage`, `LandObject`, or OSG lifetimes. Its generic grid sampling
entry point is exposed through the neutral terrain component. ESM4 terrain now resolves its default,
base, and overlay layers directly from `Land`, `LandTexture`, and `TextureSet` records, including
neutral alpha maps and explicit normal/specular paths.
The generic grid and blendmap sampling implementation now lives in `components/terrain`; the old
`components/esmterrain/gridsampling.hpp` header and its CMake entry were removed after its only
consumer moved to the neutral API.
CI now rejects a neutral bootstrap that regains those legacy service names or calls the OSG
initializer, keeping the single-backend boundary enforceable during the migration.
Engine GUI fallback frame advancement now also reads simulation time from the active
`FrameLifecycle`, keeping renderer orchestration from reaching directly into an OSG frame stamp.
The same interface now owns the engine-visible frame number: the OSG adapter reads its frame stamp,
while the Vulkan owner advances its neutral counter with simulation-frame advancement.
Loop termination is also delegated through the lifecycle; the OSG adapter preserves viewer shutdown
semantics, while a Vulkan owner can use the engine quit-request path without an OSG viewer query.
The lifecycle now exposes a renderer-neutral quit request as well: OSG forwards it to the viewer,
and Vulkan records it in its owner state. This gives future input/window services one shutdown path
without reintroducing backend-specific viewer access into the engine.
The engine simulation/update loop now owns its frame start tick and statistics sink instead of
querying the OSG viewer directly. The OSG lifecycle may still provide its viewer statistics object
for the reference renderer, while a future Vulkan owner can run the same update loop without an
OSG viewer query in `Engine::frame()`.
OSG screen-capture operation and event-handler construction is likewise limited to the OSG frame
backend; the input action remains safe when a backend has not installed that legacy capture service.
OSG profiler/resource event handlers and delayed viewer-stat reporting are also guarded by the
presence of the OSG viewer, so the engine main loop has no unconditional viewer-stat path.
Stereo management and OpenGL depth/color selection operations are now constructed only during
OSG window setup; the general engine constructor no longer allocates those OSG services.
The engine now honors a failed frame submission from `World`, retrying without advancing simulation,
focus, or frame statistics; this makes minimized and swapchain-recovery behavior part of the lifecycle contract.
The engine public header no longer imports complete OSG viewer/event-handler headers; OSG declarations
are now included only by the implementation files that use them.
SDL input transport is now renderer-neutral as well: `InputWrapper` owns only SDL state and emits
backend callbacks for frame events, function keys, and drawable resize. The OSG engine injects its
viewer event-queue callbacks, leaving a future Vulkan/input owner free to provide those operations
without linking OSG into the shared SDL input component.
Screenshot scheduling now follows the same callback boundary: `ActionManager` no longer owns an
OSG viewer or `ScreenCaptureHandler`; the reference engine supplies its capture action, while
the Vulkan presentation owner reads back its last submitted frame and writes neutral PPM, TGA, PNG,
or JPEG captures. Neutral JPEG savegame thumbnail integration is also wired; interactive GUI/video
presentation remains outstanding.
Gyroscope orientation correction now uses a scalar Z rotation and neutral float storage rather than
OSG matrix/vector types, so SDL sensor input can be reused by a non-OSG backend without importing
presentation math.
SDL video policy and gamma/window-mode handling now live behind a renderer-neutral `VideoWrapper`
callback; only the OSG GUI boundary traverses OSG windows to apply VSync. This keeps shared SDL
window policy reusable by a Vulkan presentation owner.
SDL cursor creation and window-icon conversion now consume neutral RGBA8 texture data. OSG image
sampling remains at the active engine boundary, while cursor scaling, rotation, alpha, and SDL
surface creation no longer require OSG in the reusable SDL utilities.
The engine now keeps its OSG-only depth/color selection operations local to window setup and only
creates the OSG `UnrefQueue` for the OSG backend; frame cleanup tolerates a backend without OSG
resource lifetimes.
OSG window/context creation, icon decoding, OpenGL capability discovery, and stereo setup now live
in `ViewerFrameLifecycle`; `Engine` receives only the created SDL window and capability result. This
removes the engine's direct OpenGL window-construction path and gives the future Vulkan lifecycle a
real exclusive startup boundary.
The OSG lifecycle also creates and owns the initial world scene root; `Engine` only receives a
reference while assembling OSG-specific GUI/world services.
The lifecycle also retains the discovered OpenGL texture-unit capability; `Engine` no longer stores
an OSG capability field outside the active frame owner.
World bootstrap now rejects any non-OSG lifecycle before entering `initOsgRenderer`, making the
remaining missing Vulkan game-owner path explicit instead of allowing an accidental mixed setup.
The OSG profiler and resource-statistics event handlers, plus the delayed viewer/camera statistics
report, are now owned by `ViewerFrameLifecycle`; `Engine` supplies only the existing profiler
configuration callback. This keeps viewer event-handler ownership with the OSG frame owner and
leaves the future Vulkan owner free to provide a different statistics/presentation implementation.
The viewer statistics object itself is also retained and exposed by the lifecycle; `Engine` no
longer owns an OSG stats reference and only requests the active owner’s stats for profiling.
The shared NIF file and converted-mesh caches now use a renderer-neutral `CacheManager` lifecycle;
`BaseResourceManager` extends that same contract and adds only OSG statistics/release hooks. Only
OSG-owned resource managers remain in the OSG manager list. Cache expiry, clearing, and statistics
preserve the existing behavior, while the neutral cache headers no longer import the OSG
resource-manager interface. CI checks this boundary so the Vulkan resource path cannot regain an
OSG cache dependency accidentally.

Against the current `origin/openmw-vulkan` base, the current checkpoint changes
192 files, deleting 2,034 lines and adding 12,178 lines (net `+10,144`). The larger Vulkan-only
cleanup was completed in the merged PRs #1–#5; this PR is currently a groundwork expansion,
not the speculative 10k-line reduction. The live no-GUI consumer is the first deletion
checkpoint; further reduction can now target OSG scene/resource/presentation ownership rather
than adding another compatibility bridge.

Submission validation now lives on the renderer-neutral `SceneSubmission` boundary used by the
Vulkan consumer, including dynamic mesh textures. The latest validation checkpoint also rejects non-finite scene matrices, transforms, vertex
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
callbacks; the Vulkan game owner supplies camera synchronization while bone-pose production remains
explicitly unimplemented until the animation port owns it.
RGBA8 conversion is now one renderer-neutral helper shared by image resources and terrain
blendmaps, so clamping, finite-value rejection, and byte quantization cannot drift between
resource paths. The conversion helper has direct CPU coverage.
Neutral image resolution rejects unsupported resources at the Vulkan submission boundary instead
of converting them through the legacy warning-image fallback. The RGBA8 conversion is now owned
by the neutral resource provider rather than an `ImageManager` API, so neutral resource
construction has no image-manager dependency.
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
| Full-game scene graph and world rendering | OSG reference path; Vulkan owns the experimental neutral submission path | Vulkan static and dynamic scene consumers reach parity, then delete the OSG owner |
| Vulkan validation renderer | Vulkan standalone smoke target | Retained as the migration test harness |
| Vulkan mesh submission queue | Removed | Complete |
| Inactive raster ray-tracing scaffold | Removed | Reintroduce only with a complete RT pipeline |
| Vulkan utility/queue helper paths | Removed | Complete |
| Parsed NIF resource cache wrapper | Removed | Complete; cache now owns shared NIF files directly |
| NIF-to-neutral mesh conversion | Renderer-neutral NIF boundary, material data, mesh cache, skinning metadata, dynamic mesh payloads, `SceneSubmission`, Vulkan mesh batch, and full-game neutral resolver | Add image-backed texture resolution, per-frame bone updates, and dynamic shading |
| Terrain geometry and layer data | Renderer-neutral `Terrain::RenderStorage` contract with cached per-cell LOD snapshots and a Vulkan opaque/normal/parallax/blendmap/specular layer consumer; concrete `MWRender::TerrainStorage` and legacy OSG ChunkManager remain the reference data path, including explicit ESM4 specular textures | Add quadtree-scale terrain streaming and terrain image coverage |
| Loaded-cell object identity, transforms, terrain snapshots, and paging state | Renderer-neutral `WorldScene`/`CellScene` snapshots updated by scene lifecycle; active-cell static references bypass legacy OSG paging visibility, and cell-lifecycle-cached terrain tiles flow into `SceneSubmission` | Consume snapshots from a backend and migrate visibility/paging policy |
| GUI, loading screens, screenshots, and presentation | NullWindowManager for Vulkan bootstrap; OSG/MyGUI reference path | Vulkan presentation and GUI coverage, then remove the null compatibility surface |

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

- Keep `OPENMW_USE_VULKAN` as the build gate for the Vulkan game backend and migration tests.
- Keep Vulkan translation units in the separate `openmw_vulkan` library while the remaining
  OSG engine code is reduced.
- The engine now creates one SDL window and one frame owner selected by `--vulkan`; Vulkan
  mode does not construct an OSG viewer, OSG graphics context, or OSG scene services.
- Keep the default OSG build unchanged while the Vulkan path is deliberately no-GUI.
- Add deletion guards for every OSG service that remains reachable from the Vulkan bootstrap.

### 4. Make Vulkan frame infrastructure correct

- Correct acquire/present semaphore ownership. The standalone renderer now uses per-frame acquire semaphores, per-swapchain-image presentation semaphores, and per-image in-flight fence ownership.
- Keep acquire semaphores and fences per frame-in-flight, but presentation semaphores per swapchain image.
- Handle minimized windows and zero drawable sizes without recreating a zero-sized swapchain.
- Make swapchain recreation, image layout transitions, validation layers, and resource lifetime testable.

### 5. Replace OSG scene ownership

- Separate cell visibility, transforms, camera state, lighting, and material data from OSG scene nodes. The Vulkan path now owns `WorldScene`/`CellScene` snapshots, neutral visibility for active-cell static references, neutral NIF material extraction, a cached-mesh cell composition adapter, loaded exterior terrain tiles, and a pure `collectSceneSubmission` handoff exposed by `MWWorld::Scene`.
- Keep the OSG reference path on its legacy scene graph while the Vulkan path consumes the neutral handoff.
- Delete remaining OSG scene ownership after dynamic content, presentation, and parity gates are complete.

### 6. Port static world rendering

- Wire NIF loading and `MeshConverter` into resource management. The NIF converter now has a tested tree traversal and material boundary, `NifMeshManager` caches converted instances, and `MWWorld::Scene` can collect a `SceneSubmission` containing static meshes and loaded exterior terrain without exposing OSG objects. The live Vulkan bootstrap consumes that submission; unsupported texture resources now fail at the exact neutral submission boundary instead of falling back to white.
- Implement model caching, cell add/remove, transforms, textures, materials, terrain,
  interiors, and static objects. The terrain adapter now feeds opaque and ordered
  blendmap/multi-layer Vulkan mesh consumers with normal-map sampling, height-based
  parallax, diffuse-alpha specular data, and explicit ESM4 specular textures; quadtree-scale
  terrain streaming and full terrain image coverage remain.
- Reach a static playable scene without OSG rendering, then expand neutral image coverage and
  establish camera synchronization.

### 7. Port dynamic content and presentation

- Add actors, skinning, animation, particles, weather, water, spell effects, and post-processing.
- Port GUI, fonts, loading screens, cursor handling, and video presentation. Vulkan now has a
  deliberately small PPM/JPEG/PNG screenshot path for visual checkpoints and neutral JPEG savegame
  thumbnails; interactive GUI/video presentation remains outstanding.
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

The full game should not be repeatedly started for every change. The main engine loop, loading
screen, modal/video loops, and screenshot capture now delegate frame advancement, event processing,
update traversal, and frame submission through one engine-owned frame lifecycle. The engine creates
one OSG `ViewerFrameLifecycle` before world initialization; that lifecycle owns the OSG viewer and
the engine passes its frame service explicitly to `World`. The legacy manager receives only screenshot
render/advance callbacks and stores no frame lifecycle.
Direct OSG frame operations remain only in that adapter, while bootstrap callbacks use the same small
frame-owner type that can be replaced with the Vulkan presentation owner. This establishes the
replacement point for a future Vulkan frame owner while current OSG behavior remains unchanged.
The interface now has an explicit submission-consuming path: the Vulkan owner
receives a synchronized `SceneSubmission` from `World` and validates it before upload, while the OSG owner
explicitly reports that it does not consume submissions and continues its legacy traversal.
Submission validation remains at the single Vulkan world/frame-owner boundary; the OSG path does
not construct or export a duplicate neutral scene.
