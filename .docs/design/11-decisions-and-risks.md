# Decisions and risks

## Decision record policy

These decisions are accepted for the MVP. A change that invalidates an architecture invariant, requirement, release limit, public registration identity, or parser/security boundary must update this document and the affected design/test evidence in the same change.

## ADR-001 — native Win32/C++ viewer

**Status:** Accepted by product pivot.

The interactive viewer is a standalone x64 Win32/C++20 application using Direct3D 12. Flutter, Dart isolates/FFI, Chromium, and a hybrid UI runner are removed from the target architecture. A separate zero-capability AppContainer native import host is permitted only for lazy OpenUSD compatibility work and has no UI or persistent lifetime.

Reason: startup, frame ownership, memory mapping, cancellation, copy-queue submission, residency, and device recovery all need one native lifetime/type system and explicit scheduling. This also removes the original rationale for a warm Flutter daemon.

Consequence: the product owns native window chrome, accessibility, layout/overlay, graphics synchronization, and crash handling. The Visual Studio wizard project is scaffolding, not the final structure.

## ADR-002 — separate UI, render, upload, and worker lanes

**Status:** Accepted.

The UI thread owns only HWND/message/input/AppState. The render thread owns swap-chain presentation and the direct queue. One upload coordinator owns copy submission/ring retirement. A bounded adaptive worker pool owns cache/mapping/parsing/normalization/LOD/texture CPU work. A loader-owned broker exclusively controls the optional compatibility-host process and its shared sections.

Reason: moving parsing off the UI thread is insufficient if Present, resizing, default-heap allocation, or a queue wait can still block it. Exclusive queue/list ownership also makes fence lifetime auditable.

Consequence: cross-lane communication uses bounded value/handle queues and immutable snapshots. Some additional latency of one frame is preferable to shared mutable scene state.

Rejected: running Present in the UI message pump; permitting arbitrary workers to record/submit copy lists; one coarse global mutex around a mutable scene.

## ADR-003 — mapped input is a bounded I/O primitive, not a zero-memory promise

**Status:** Accepted.

Tier A sources use CreateFileMapping/MapViewOfFile and mapping-window leases. The implementation does not issue an eager source-sized read or retain unbounded normalized arrays.

Reason: mapping lets the OS page cache service validated ranges and avoids a redundant complete file buffer. However, touched pages still consume memory and most formats require normalized/decompressed CPU and GPU data.

Consequence: mapped bytes never go directly to a draw call. Budgets cover scratch, archive expansion, textures, upload heap, default heap, and cache separately. Raw source pointers cannot outlive a lease.

Rejected: std::ifstream/read-whole-file ingestion for large assets; describing MapViewOfFile as preventing all out-of-memory failures.

## ADR-004 — publish only fence-complete GPU resources

**Status:** Accepted.

A resource becomes part of a renderable SceneSnapshot only after the copy fence covering all of its bytes and state preparation has completed. The direct queue normally draws the prior LOD rather than waiting for an incomplete upload.

Reason: a direct-queue Wait on every streamed copy serializes visible progress behind transfer and can turn asynchronous ingestion into stutter. Fence-complete publication gives a simple lifetime invariant.

Consequence: parent/proxy LODs stay alive longer and snapshots/deferred release need dual-fence accounting. Rare unavoidable cross-queue waits require a measured exception.

Rejected: atomically swapping a raw MeshInstance immediately after copy submission; making the direct queue wait for the next detail chunk each frame.

## ADR-005 — proxy plus view-prioritized residency

**Status:** Accepted.

Tier A imports build an always-resident coarse proxy and bounded spatial detail chunks. Fine LOD/texture mips are requested by visible error and evicted against the current DXGI budget.

Reason: multi-gigabyte source data can exceed local video memory even when ingestion is perfectly asynchronous. Uploading the complete model is not a viable product contract.

Consequence: full detail may never be simultaneously resident. The UI accurately reports reduced/refining detail. Chunk-local origin rebasing and parent-until-child-ready rendering are required.

Rejected for MVP: fail when full geometry does not fit; automatic whole-model downsample only; virtual-texture/Nanite-like hierarchy, mesh shaders, DirectStorage, or an unbounded disk cache.

## ADR-006 — format-specific importer stack

**Status:** Accepted, with Gate 3/4 validation items below.

| Family | Choice | Rationale |
| --- | --- | --- |
| glTF/GLB | fastgltf + Google Draco + KTX/Basis + libwebp | glTF-specific validation/data sources, common geometry/texture compression, and mapped-file-friendly API |
| STL/PLY | Product parsers | grammars are bounded enough for controlled binary streaming, safe text tokenization, and representative proxy sampling |
| OBJ/MTL, FBX | ufbx | single bounded C implementation, explicit allocation/progress controls, static FBX and OBJ/MTL support |
| 3MF | lib3mf | official 3MF Consortium implementation/API with required preview extensions |
| USD/USDA/USDC/USDZ fast path | TinyUSDZ | small common static subset, packaged-format support, and memory budget |
| broader USD compatibility | OpenUSD in AppContainer host | materially broader composition while keeping weight, crashes, paths, and memory outside the viewer |
| expanded raster textures | DirectXTex/inbox WIC/libwebp | deterministic app-owned BMP/TIFF/TGA/DDS/HDR/WebP coverage without arbitrary installed codecs |
| LOD/mesh compression | meshoptimizer | bounded cluster simplification and supported EXT_meshopt decode |
| GPU allocation | D3D12 Memory Allocator | suballocation/budget utilities without ceding product policy |

Reason: no single broad importer has the best performance, limits, fidelity, dependency weight, and attack surface for every format.

Consequence: Model Core owns one adapter contract and consistent budgets/errors. Library-native objects and exceptions do not cross it. OpenUSD objects do not cross the process boundary. Each dependency gets its own fuzz/conformance gate, and compressed payloads have decoded-size limits independent of source size.

Rejected: Assimp as a universal runtime importer; Autodesk FBX SDK due package/redistribution/weight concerns; loading OpenUSD into the viewer or thumbnail provider; using arbitrary installed WIC codecs.

Primary upstream references: [fastgltf](https://github.com/spnda/fastgltf), [Google Draco](https://github.com/google/draco), [KTX-Software](https://github.com/KhronosGroup/KTX-Software), [ufbx](https://github.com/ufbx/ufbx), [lib3mf](https://github.com/3MFConsortium/lib3mf), [TinyUSDZ](https://github.com/lighttransport/tinyusdz), [OpenUSD](https://openusd.org/release/), [DirectXTex](https://github.com/microsoft/DirectXTex), [meshoptimizer](https://github.com/zeux/meshoptimizer), and [D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator).

## ADR-007 — CPU Explorer thumbnails

**Status:** Accepted.

The in-process COM provider renders a deterministic, sampled mesh/point-cloud thumbnail on the CPU and creates no D3D device. It may use the strictly bounded in-process decoders but never starts the OpenUSD compatibility host or accesses the viewer cache.

Reason: driver/device creation and asynchronous GPU lifetime inside Explorer's surrogate add failure modes disproportionate to a small thumbnail. CPU sampling has a clear deadline and memory cap.

Consequence: thumbnail shading is an approximation and external sidecars are unavailable through IInitializeWithStream. Explorer may show a generic icon when safe bounded import is impossible.

Rejected: launching the viewer to capture a frame; IPC to a background renderer; GPU creation within the provider; reading arbitrary siblings based on untrusted embedded paths.

## ADR-008 — no warm daemon or tray

**Status:** Accepted.

The native viewer cold-starts when needed and exits after the last window closes. It forwards opens only while a visible primary instance exists. The compatibility host starts only on demand for a current USD generation and is terminated at generation/idle/shutdown boundaries; it is not a warm daemon.

Reason: native startup removes the Flutter VM cold-start pressure that motivated the daemon. Zero background footprint and simpler update/security/lifecycle behavior outweigh a speculative few milliseconds.

Consequence: cold startup is a formal 150/200 ms performance gate. IPC remains for visible-instance reuse and replacement semantics, not persistence.

Rejected: hide-on-close, start-at-login, system tray exit menu, helper launcher service.

## ADR-009 — system D3D12 runtime and feature level 11_0

**Status:** Accepted.

The MVP targets Windows 11's system D3D12/DXGI runtime and hardware feature level 11_0 or newer. It uses conservative descriptor tables and offline DXC shaders.

Reason: this covers the target OS without shipping an additional runtime contract, and it avoids making bindless/mesh-shader-era hardware a minimum requirement.

Consequence: modern features that require the Agility SDK or later binding tiers are not assumed. A later feature need can revisit packaging with a new ADR.

Rejected for MVP: automatic WARP user fallback; Agility SDK solely for novelty; D3D11 primary renderer.

## ADR-010 — D3D11On12/Direct2D for compact overlays

**Status:** Accepted provisionally through Gate 1.

The render thread uses D3D11On12 plus Direct2D/DirectWrite over the same direct queue for title/status/error text and controls, after D3D12 scene commands and before final frame-fence signal/present.

Reason: native text layout, scaling, high contrast, and typography are costly to recreate correctly as a custom glyph engine. The overlay surface is small and changes infrequently.

Consequence: one render thread must own all interop contexts and follow Acquire/Release/Flush ordering. Gate 1 profiling may replace this with a product glyph atlas only if interop demonstrably violates frame/lifetime gates; accessibility remains UI Automation either way.

Rejected: rendering the 3D scene through D3D11; overlay calls from the UI thread; runtime HTML UI.

## ADR-011 — signed per-machine MSI, user-controlled defaults

**Status:** Accepted.

The app and seven stable thumbnail CLSIDs are installed machine-wide by MSI. Capabilities, Open With ProgIDs, and thumbnail handlers are registered; setup never overwrites UserChoice/default-app selection.

Reason: COM shell registration needs stable installation paths and repair/uninstall ownership. Windows explicitly controls defaults.

Consequence: elevation is required, handler conflicts are preserved/reported, and install lifecycle receives its own VM gate.

Rejected: self-registration with regsvr32, application-side first-run registry mutation, forcing defaults, portable ZIP as MVP distribution.

## ADR-012 — bounded persistent derived-data cache

**Status:** Accepted for the revised MVP.

The viewer keeps a default-enabled, per-user, bounded cache of verified coarse proxies, normalized chunk/catalog data, and eligible decoded/transcoded texture levels. Entries are opaque, versioned, checksummed, source-version-bound, LRU-evicted, and revalidated as untrusted input. Fast hits require stable local file/change-journal version tokens; otherwise a full content digest is required or the entry is a miss. The user can disable and clear the cache. The thumbnail provider does not access it.

Reason: repeat parsing, simplification, texture decoding, and compatibility-host startup dominate reopen latency for medium/large assets. Caching derived data produces a larger real-world improvement than further optimizing already asynchronous UI startup.

Consequence: the viewer owns up to a 10 GiB soft-cap of renderable model-derived data under LocalAppData, must document that privacy/storage fact, stop admission under low disk space, survive corrupt/hostile entries, and preserve cache-schema compatibility deliberately. Cache writes are never on the critical path to first geometry or Ready.

Rejected: an unbounded cache; storing original source bytes or full paths/names; sharing writable cache state with Explorer; trusting cache records because the current user owns them; deleting arbitrary user-profile data from elevated MSI code.

## ADR-013 — hybrid USD import with an AppContainer compatibility host

**Status:** Accepted for the revised MVP.

TinyUSDZ remains the in-process common-static fast path. A typed unsupported-composition result may start `Preview3DImportHost.exe`, which links pinned OpenUSD, runs under a zero-capability AppContainer token in a kill-on-close Job Object, resolves only parent-brokered read-only local assets, and returns bounded normalized chunks through revalidated shared sections.

Reason: generic USD claims are not credible with the TinyUSDZ subset alone, while loading the full OpenUSD stack into the viewer would increase startup weight, plug-in/path authority, memory, and crash impact for every format.

Consequence: the installed product gains another signed executable and app-local dependency payload, a security protocol boundary, process/job lifecycle, and a second USD conformance path. The host is lazy and nonpersistent; a failure affects only the current document. USD remains a documented static preview subset rather than complete authoring fidelity.

Rejected: generic USD marketing backed only by TinyUSDZ; in-process OpenUSD in the viewer or Shell provider; giving the host unrestricted filesystem/network access; a permanent import daemon.

## Invariants

The following are release blockers if violated:

1. UI thread never maps/parses/normalizes/decodes/allocates GPU resources, submits queues, presents, waits on a worker/fence/device idle, or joins during visible operation.
2. Render thread never waits for ordinary in-progress content; it selects an already Ready representation.
3. Only the upload coordinator mutates the upload ring and submits the copy queue.
4. Only fence-complete resources enter renderable snapshots; old resources survive through last-use fences.
5. Every queue, allocation, archive, graph, image, and source has an enforced bound and cancellation path.
6. A source-derived path cannot escape the selected local model directory or trigger network access.
7. A multi-gigabyte Tier A model does not require source-sized private commit or full-detail VRAM residency.
8. Shell code launches no process, opens no sidecar/path, creates no GPU device, and writes no persistent model data.
9. Closing the visible app terminates the viewer and any compatibility host; install/uninstall never take control of user defaults.
10. Persistent cache data is never trusted without version, identity, length, checksum, and normalized-scene validation; a cache failure is equivalent to a miss.
11. OpenUSD runs only in the zero-capability AppContainer compatibility host; all dependencies are parent-brokered and all returned sections are revalidated before upload.

## Risk register

| ID | Risk | Probability / impact | Mitigation and evidence | Trigger / contingency |
| --- | --- | --- | --- | --- |
| R-01 | Copy queue shares hardware with graphics, so DMA does not overlap | Medium / Medium | Correctness never assumes overlap; bounded batches; frame/copy ETW on discrete and UMA | If copy batches hurt frames, reduce batch/time slice and staging target; retain old LOD longer |
| R-02 | A Tier B library creates large intermediate state | High / High | Allocation callbacks/host limits, Tier B caps, early spike/peak-memory tests, transient normalized store | Lower per-format limit or replace adapter; never relax global memory invariant |
| R-03 | TinyUSDZ subset misses required USD assets or differs from OpenUSD normalization | Medium / High | typed fallback, overlap corpus, explicit feature matrix, AppContainer OpenUSD host | Route supported composition to host; narrow documented USD subset if results cannot be made consistent |
| R-04 | Complete proxy misses 5-second target because bounds/simplification require full scan | Medium / High | source-order-independent stratified sampler, parallel bounded scan, early partial proxy telemetry | Tune proxy algorithm/chunking; maintain partial usefulness while treating gate miss as blocker |
| R-05 | Transient/persistent derived stores consume disk or add latency | Medium / High | delete-on-close transient cap; 10 GiB/2 GiB persistent caps, free-space floor, atomic writes, background LRU | Stop cache admission/trim; fall back to uncached or coarse-only without delaying the viewer |
| R-06 | OS video budget drops below reserved proxy set | Medium / Medium | Continuous budget notification, proxy density tiers, low-mip fallbacks | Rebuild smaller proxy and disclose reduced detail; fail only if minimum render set cannot fit |
| R-07 | D3D11On12 overlay introduces synchronization stalls | Medium / Medium | Same-thread ownership, correct final fence, Gate 1 PIX/validation | Replace overlay backend behind UI renderer interface via ADR update |
| R-08 | Parser defect crashes Explorer surrogate | Medium / Critical | stricter stream caps/deadline, CPU path, fuzz/ASan/actual-surrogate soak, no shared state | Disable affected extension handler in servicing release while viewer remains available |
| R-09 | Existing thumbnail handler or locked surrogate complicates MSI | Medium / Medium | non-clobber registration, Restart Manager, unloadable DLL, rollback VM matrix | Install viewer only for conflict; exceptional 3010 rather than terminate Explorer |
| R-10 | Reference corpus understates real CAD topology/material complexity | Medium / High | anonymized representative acquisitions, adversarial graph/layout fixtures, published dimensions | Revise workload and gate before claiming support; no benchmark-specific special case |
| R-11 | Huge coordinates lose precision after float GPU conversion | Medium / Medium | double CPU transforms/bounds, cluster origins, camera-relative matrices, precision fixtures | Reduce spatial cluster size or split transforms further |
| R-12 | Malicious geometry triggers TDR/very long GPU work | Low / High | bounded chunk draw counts, validated commands, proxy density, GPU timing and device recovery | Reduce draw batch complexity/detail; blacklist pathological chunk with visible warning |
| R-13 | Parser/dependency vulnerability or abandoned upstream | Medium / High | pin/SBOM/security watch, wrapper/fuzz coverage, replaceable adapter interfaces | Patch/vendor fix or disable affected format until signed update |
| R-14 | First-window 150 ms target is missed by signing/AV/device startup | Medium / Medium | show native brush before device, lazy nonessential init, measure cold with common security software | Move additional initialization behind first present; revise target only with product approval/evidence |
| R-15 | CPU thumbnail misses quality/time target on complex packages | Medium / Medium | representative sampling, fixed deadline, golden/perf corpus | Reduce triangle sample/supersampling; safely return generic icon |
| R-16 | OpenUSD payload size/startup or host composition misses medium-file targets | Medium / High | lazy launch, `LoadNone`, brokered incremental payloads, warm derived cache, exact performance corpus | keep common stages on TinyUSDZ; lower composed-stage limits or document slower compatibility tier |
| R-17 | Compatibility host escapes its file/network/process boundary or returns hostile shared data | Low / Critical | zero-capability AppContainer, kill-on-close Job Object, broker-only resolver, protocol/section revalidation, fuzz and penetration tests | disable OpenUSD fallback in servicing release until containment is restored |
| R-18 | Persistent cache discloses derived model content or serves stale/corrupt geometry | Medium / High | current-user ACL, opaque keys/no names, file/change-journal version token or full digest, SHA-256 sections, normalized validation, user disable/clear | disable cache, invalidate schema, and purge affected entries on next launch |
| R-19 | Draco/KTX2/WebP or expanded texture decode creates an expansion/OOM attack | Medium / High | per-payload decoded limits, pinned hardened decoders, cancellation, fuzz corpora, no decoder-global cache | fall back only for optional textures; reject required geometry and disable affected decoder if needed |
| R-20 | PLY/point-cloud or Beam Lattice workloads create excessive primitives/draw cost | Medium / Medium | bounded list/tessellation, point proxy sampling, chunk draw caps, draw-heavy GPU timing | lower point/lattice density, preserve representative proxy, report reduced detail |

## Required validation spikes

These tasks validate implementation choices; they do not expand scope:

1. Before Gate 1 completion, measure D3D11On12 overlay ordering and cost at 144 Hz, including resize and GPU validation.
2. Before Gate 3 implementation, confirm exact fastgltf API/version support for mapped sources, sparse accessors, `KHR_mesh_quantization`, and `EXT_meshopt_compression`; separately measure bounded Draco decode and KTX2/Basis transcode on the compressed corpus.
3. Before Gate 3 claims Tier-A PLY, spike binary little/big-endian sequential scanning, stratified proxy quality, point rendering, hostile list limits, and 2–4 GiB memory behavior.
4. At Gate 4 start, build capped-memory/cancellation spikes for ufbx static skin/blend evaluation, lib3mf Beam Lattice tessellation, and TinyUSDZ before connecting them to the viewer.
5. Before the OpenUSD slice, prove the AppContainer host's pinned minimal payload, `LoadNone` startup, brokered resolver, Job Object limits, cancellation/termination, shared-section validation, and overlap normalization corpus. Generic “USD support” must never imply all OpenUSD semantics.
6. Before enabling persistent writes, prove file/change-journal token availability and invalidation, journal-reset and full-digest fallback cost, corrupt-entry fuzzing, atomic crash recovery, LRU/free-space behavior, warm-open value, and Clear cached previews races.
7. Before Gate 6, benchmark mesh/point and compressed glTF CPU rasterizer prototypes inside the actual thumbnail surrogate.
8. Before Gate 7, validate thumbnail-handler conflict, loaded-DLL/host upgrade behavior, signed/hash-verified OpenUSD payload search lockdown, and cache preservation/removal documentation on clean Windows 11 VMs.

If a spike fails, the team updates the corresponding ADR, product limits, and acceptance test before implementation continues. It may not quietly substitute an unbounded importer or a render-thread wait.
