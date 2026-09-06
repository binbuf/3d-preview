# System architecture

## Runtime topology

The installed product has three runtime components: the native viewer EXE, a lazily started AppContainer compatibility-host EXE, and the Explorer thumbnail COM DLL. The viewer and provider link the bounded fast-path adapters; the compatibility host alone links OpenUSD and its pinned app-local support payload.

```text
Explorer thumbnail host                         Preview3D.exe
        |                                           |
        | IStream                                   +-- Win32 UI thread
        v                                           |     message/input/state only
Preview3DThumbnailProvider.dll                      |
        | stream-buffer adapters                    +-- Render thread
        v                                           |     direct queue/swap chain/present
model-core parsers -> CPU rasterizer                |
                                                    +-- Loader worker pool
                                                    |     map/parse/normalize/LOD/texture
                                                    |
                                                    +-- Upload coordinator
                                                          upload ring/copy queue/fence
                                                                 |
                                                     completed chunks only
                                                                 v
                                                      render scene/residency manager

Preview3D.exe -- brokered handles/shared sections --> Preview3DImportHost.exe
                                                       AppContainer OpenUSD import
                                                       bounded normalized chunks
```

There is no runtime IPC between the thumbnail DLL and either executable. The provider never loads or launches the viewer or compatibility host. The viewer contains no COM thumbnail object. `Preview3DImportHost.exe` is started only for a USD generation that exceeds the TinyUSDZ fast subset and exits after that generation or an idle grace period; it is never a daemon or Shell child.

## Execution lanes

### Win32 UI thread

The `wWinMain` thread initializes COM as STA, establishes singleton ownership, starts the IPC service, creates/shows the top-level HWND, and runs `GetMessage`. It owns:

- window lifetime, non-client hit testing, DPI/theme/accessibility notifications;
- immutable input snapshots and high-level app/load state;
- open dialog, drag/drop admission, keyboard commands, and error/setting commands;
- `PostMessage`/bounded-queue dispatch to other lanes.

It never parses, maps large asset regions, decodes textures, records/submits D3D12 commands, calls `Present`, waits for a fence, or joins a load thread during normal operation.

### Render thread

One dedicated thread initializes DXGI/D3D12, owns the direct command queue, swap chain, frame contexts, depth/render targets, graphics PSOs/root signatures, draw-list consumption, resize, and `Present`. It renders from immutable scene snapshots published at frame boundaries. It polls completed copy/direct fences without blocking the UI.

Only this thread mutates the active render scene and descriptor-visible draw records. GPU resources use deferred release tagged with the last direct-fence value that can reference them.

### Loader pool

A bounded `std::jthread` pool uses MTA COM where WIC is needed. Pool width is calculated with saturating arithmetic from available logical processors, initially leaving at least two logical processors outside the pool and capped at eight. It can be lowered when frame latency, input latency, power state, or memory pressure crosses policy thresholds. Loader work runs below UI/render priority. Jobs perform cache validation, mapped I/O, parser work, validation, normalization, bounds, chunk construction, texture decode/transcode/mip generation, and LOD/proxy generation.

Every load has a generation and `stop_source`. Work checks cancellation between bounded units and uses parser-native progress/cancellation where available. Results go through bounded queues; workers may backpressure, never allocate an unbounded result backlog.

### Compatibility host

The viewer owns a broker thread that may start one `Preview3DImportHost.exe` for the current USD generation. The host runs under a zero-capability AppContainer token and a kill-on-close Job Object with commit, process-count, CPU-time, and child-process limits. The AppContainer SID has read/execute access only to the installed host/OpenUSD payload and explicit access to per-generation broker objects. It has no network capability, inherited directory handle, direct model-directory access, current-directory search, or environment/model-selected plug-in path.

The parent opens the primary file and approved local dependencies, then duplicates read-only handles or serves bounded ranges through a versioned private IPC channel. A product-owned OpenUSD asset resolver requests dependencies from this broker; arbitrary filesystem opens and URI schemes are denied. Normalized metadata and chunk bytes return through bounded shared-memory sections whose headers, offsets, counts, generation, and checksums are revalidated by the viewer before entering the ordinary upload path. OpenUSD objects and pointers never cross the process boundary.

The host is not on the window-startup path. Cancellation closes generation channels and Job Object handles after a short cooperative grace period. A crash, timeout, protocol violation, or limit terminates only the host, invalidates its shared sections, and produces a recoverable typed import error. The viewer may create a fresh host for a later generation but does not retry the same failing stage in a loop.

### Upload coordinator

One dedicated thread is the sole owner of copy-queue submission and upload-ring suballocation. Workers enqueue immutable `UploadBatch` descriptions plus CPU spans whose lifetime is explicit. The coordinator:

1. polls/reclaims ring ranges by copy-fence value;
2. waits only on its own event when the ring is full (UI/render continue);
3. copies normalized bytes into the persistent upload mapping;
4. records a fresh/reset copy command list and submits it serially;
5. signals a monotonically increasing copy fence;
6. moves the batch to a pending-completion queue;
7. publishes `ResidentChunk` only after `GetCompletedValue` reaches the batch fence.

Command allocators/lists are never reset until their fence is complete and are never recorded concurrently by two threads.

## Thread communication and ownership

| Payload | Producer → consumer | Mechanism | Ownership rule |
| --- | --- | --- | --- |
| Input/window snapshot | UI → render | double-buffered atomic index | render reads immutable copy |
| Load request/cancel | UI → loader coordinator | bounded MPSC queue + stop token | generation owns job graph |
| Bounds/progress/error | workers → UI | bounded queue + one coalescing `PostMessage` | small value objects only |
| Compatibility import request | loader broker → import host | private authenticated pipe + duplicated read-only handles | one current USD generation; no path authority in child |
| Compatibility normalized chunk | import host → loader broker | bounded shared section + control message | parent validates header/ranges/checksum before accepting bytes |
| Derived cache lookup/write | loader → cache coordinator | background file I/O + atomic manifest replacement | cache data is untrusted, GPU-independent, and generation/version bound |
| Normalized upload batch | workers → upload | bounded MPSC queue | CPU backing through ring memcpy; ring range through copy fence |
| Fence-complete chunk | upload → render | bounded SPSC queue | render assumes GPU copy complete |
| Residency request/eviction | render → loader/upload | bounded priority queue | scene/chunk ID, never raw UI pointer |
| Scene snapshot | render-owned scene builder → render | atomic `shared_ptr<const ...>` swap at frame boundary | immutable; deferred GPU deletion |

No queue stores references into a remappable file view beyond the mapping lease lifetime. Raw HWND access is confined to UI/render integration code; workers post IDs/value objects.

## Components

| Component | Responsibility |
| --- | --- |
| `platform` | RAII handles/mappings, checked math, paths, clocks, thread naming, errors |
| `app` | startup, state machine, command routing, active-instance IPC, cancellation |
| `model-core` | format sniffing, parser adapters, resource resolver, normalized scene/chunk schema |
| `streaming` | source leases, transient normalized store, persistent derived cache, proxy/LOD builder, chunk priorities |
| `import-host` | AppContainer OpenUSD stage composition and normalized shared-section production |
| `graphics` | adapter/device, queues/fences, D3D12MA, upload ring, descriptors, renderer, DRED |
| `ui` | custom window chrome, DirectWrite text/glyphs, overlays, accessibility providers |
| `thumbnail` | COM class factory/provider, stream adapter, bounded import, CPU rasterizer |
| `installer` | MSI files, COM/Shell/app registration, signing and lifecycle |

## Dependency choices

All versions/revisions are exact-pinned in Gate 0 and may change only through dependency review; no floating branch is a release input.

| Dependency | Use | Boundary |
| --- | --- | --- |
| fastgltf | GLB/glTF 2.0 JSON/metadata/accessors | viewer mapped adapter; provider memory-buffer adapter |
| Google Draco decoder | `KHR_draco_mesh_compression` | bounded cancellable decode jobs; no encoder in runtime |
| KTX-Software/Basis transcoder | KTX2 and `KHR_texture_basisu` | bounded worker transcode to supported BC/RGBA formats |
| libwebp | deterministic WebP decode | worker/provider memory-buffer adapter where format policy permits |
| ufbx | FBX plus OBJ/MTL | Tier-B adapter with its allocation/progress limits enabled |
| lib3mf | 3MF package/Core model | Tier-B adapter; package/expanded limits applied before/through reader |
| TinyUSDZ | USDA/USDC/USDZ common static subset | in-process Tier-B fast adapter with production build and memory budget |
| OpenUSD | broader local static USD composition | AppContainer compatibility host only; custom brokered resolver and pinned signed payload |
| meshoptimizer | vertex-cache optimization, chunk meshlets/clusters, simplification/proxy LODs | bounded normalized chunks only; experimental APIs excluded unless separately gated |
| D3D12 Memory Allocator | placed-resource/default-heap allocation and budget statistics | graphics module only |
| DirectXTex/WIC | PNG/JPEG/BMP/TIFF/HDR/TGA/DDS decode, resize, mip generation, supported GPU formats | worker threads; inbox WIC codecs only, no runtime-installed codec discovery |
| DirectXMath | camera/transforms/bounds | product math layer |
| DXC | offline HLSL → DXIL | build-time only; compiled shader blobs embedded/packaged |
| WIL/WRL | Win32/COM resource ownership | no exception crosses COM/Win32 callbacks |

The viewer and thumbnail DLL do not link Assimp, Flutter, Chromium, a scripting runtime, OpenUSD, or an FBX SDK. The compatibility host alone links OpenUSD; it cannot load arbitrary plug-ins or expose OpenUSD objects to the viewer. Format adapters expose product-owned data types so third-party structures never reach graphics/UI interfaces.

## End-to-end viewer flow

1. UI validates extension/path and posts `OpenModel(generation, path)`.
2. Loader opens the file read-only with a stable share policy, records file identity/size, and creates a read-only file mapping.
3. The fast adapter performs a cheap metadata/dependency preflight. It publishes provisional bounds only if validated metadata supplies them and opens every geometry-affecting local dependency through the ordinary path policy.
4. Cache coordinator derives an opaque key from the primary and dependency version evidence and validates any candidate manifest, schema/importer versions, lengths, and checksums. A fast hit requires stable local file/change-journal tokens; otherwise a full content digest is required or the candidate becomes a miss. A valid coarse proxy may publish through the normal upload path while optional cached detail is admitted by view priority.
5. Workers parse into bounded normalized chunks. Full-source heap materialization is forbidden for Tier A. Binary STL and supported binary PLY layouts use sequential windows/prefetch; sparse glTF keeps random/windowed access.
6. TinyUSDZ first attempts the documented common USD subset. A typed `UnsupportedComposition` result asks the broker to start the zero-capability AppContainer OpenUSD host; malformed or unsafe input is never retried as a compatibility fallback.
7. Chunk builder computes verified bounds, normals/tangents only where needed, proxy/LOD and material references. Tier-B adapters may write normalized chunks to a delete-on-close mapped transient store to cap RAM.
8. Upload coordinator stages the smallest useful proxy and low-resolution or precomputed texture mips first, then visible/high-value detail.
9. Fence-complete chunks are published to render; render swaps scene/draw snapshots only at a frame boundary.
10. Residency manager continuously admits/evicts fine chunks against current DXGI budget. Proxy resources stay high priority/resident.
11. Once verified, the cache coordinator atomically commits a GPU-independent coarse proxy/catalog and eligible reusable chunks without delaying Ready. Replacement/cancel makes old-generation work drop results; render retires its scene only after a replacement proxy is ready unless memory pressure requires early release.

## Failure containment

| Failure | Required behavior |
| --- | --- |
| map/read/parser/sidecar failure | cancel that generation; UI/render continue; show stable error |
| stale/corrupt/full derived cache | treat as miss, quarantine/delete invalid entry when safe, continue uncached without UI/render wait |
| compatibility-host crash/timeout/protocol fault | terminate host job, discard sections, fail only that generation; a later open may create a fresh host |
| worker allocation limit | lower optional detail where valid or return resource error |
| upload-ring full | upload thread backpressures; no UI/render wait |
| default-heap allocation/budget drop | evict fine LODs, reduce texture mips, retain proxy/UI |
| copy failure/device removal | stop publication, collect DRED, request render-owned device recovery |
| direct device removal | render falls back to UI error surface, recreates once, requests reupload from chunk store |
| later activation | cancel prior generation; stale messages/resources are reclaimed by generation/fences |
| thumbnail parser failure | HRESULT failure inside Shell isolation; no viewer impact |

## Intended repository layout

```text
Directory.Build.props/targets       shared compiler/link/security policy
3d-preview-windows.slnx             root solution
vcpkg.json                          dependency manifest
vcpkg-configuration.json            pinned registry baseline
.docs/design/                       normative documents
shared/model-core/
  include/                          product-owned parser/chunk contracts
  src/                              format adapters, resolver, normalization
shared/platform/                     Win32 RAII, checked math, paths, mapping
compatibility-host/
  src/                              broker protocol/AppContainer OpenUSD adapter
  Preview3DImportHost.vcxproj
interactive-viewer/
  src/app/                          entry point, state, IPC, commands
  src/platform/                     Win32 window, mapping, RAII, accessibility
  src/graphics/                     D3D12 device/queues/ring/render/residency
  src/streaming/                    workers, chunks, LOD/cache
  src/ui/                           chrome/overlays/text
  shaders/                          HLSL sources
  Preview3D.vcxproj
thumbnail-provider/
  src/                              COM provider and CPU rasterizer
  ThumbnailProvider3D.vcxproj
installer/                          WiX project/registration
tests/
  unit/                             core/platform/graphics/COM unit tests
  integration/                      viewer, IPC, GPU, Shell-host scenarios
  corpus/                           manifests, small licensed fixtures, regressions
  performance/                      large-fixture generators and benchmark harness
  fuzz/                             per-boundary fuzz targets
  compatibility-host/              broker, shared-section, restriction, and crash tests
third_party/notices/                pinned dependency notices
scripts/                            reproducible build/test/package commands
```

The current wizard-generated `Preview3D.cpp` and thumbnail `dllmain.cpp` are scaffolds, not architecture. Implementation removes global mutable application variables and splits responsibilities before feature work. Only x64 configurations ship; Win32 project configurations are removed or explicitly non-buildable to prevent accidental packaging.
