# User experience

## Scope of this document

This document describes the UI/UX of the **current implemented vertical slice** (`interactive-viewer`, GLB-only), not a forward-looking aspiration. The team spent a dedicated pass on window chrome, camera feel, and information display before broad format/cache/accessibility work started, and the product's visual and interaction identity now comes from that pass. Other design documents describe the target multi-format ingestion, caching, and accessibility architecture ([01](./01-product-scope.md), [03](./03-file-formats-and-ingestion.md), [04](./04-rendering-and-streaming.md)); where this document's current behavior is narrower than those targets (format support, persistence, accessibility), it says so explicitly rather than describing unbuilt behavior. Widening format/cache/accessibility support in later gates must preserve the window, chrome, and camera behavior fixed here unless a change is deliberately proposed and recorded.

## Experience principles

The viewer should feel like a small, fast, native tool: a custom dark window with no MDI/browser chrome, one document at a time, an immediately responsive camera, and information available on demand rather than always on screen. Loading is a single indeterminate phase today — there is no progressive-detail promise yet — but it must never block camera interaction with whatever model is already on screen.

## Window

The window opens at 1000 × 720 logical pixels, centered on the primary monitor's work area, with a 480 × 360 logical-pixel minimum enforced through `WM_GETMINMAXINFO`. Size/position is not persisted between launches. DPI awareness is per-monitor-v2; `WM_DPICHANGED` resizes to the suggested rect and rebuilds the chrome font and layout.

The window background — both the window-class brush shown before the first swap-chain frame and the D3D12 clear color — is **#1C1C1E**, not a lighter neutral. There is no visible flash between the native background and the first rendered frame.

Chrome is a real `WS_OVERLAPPEDWINDOW` with the non-client caption area removed in `WM_NCCALCSIZE`/`WM_NCHITTEST` and passed through `DwmDefWindowProc`, so resize borders, Snap Layouts, Alt+Space, and the system move/size behaviors keep working even though the entire visible title bar is custom-drawn. `DWMWA_USE_IMMERSIVE_DARK_MODE` is set, corners use `DWMWCP_ROUNDSMALL` while windowed and `DWMWCP_DONOTROUND` while fullscreen, and caption/border colors are set to #242426/#3A3A3C through the corresponding DWM attributes.

**Fullscreen** (F11, or the bottom-bar fullscreen button) is a distinct mode from Maximize: the window expands to the exact monitor bounds, becomes topmost, hides the taskbar, and both toolbars collapse their viewport insets to zero while still drawing as floating overlays. Maximize snaps to the work area normally (taskbar visible, not topmost). Esc or F11 exits fullscreen; Esc otherwise cancels an in-progress open.

## Chrome

All chrome except three error-state buttons is drawn with Direct2D/DirectWrite over the D3D12 scene, not native HWND controls.

### Title bar (52 logical px)

Left to right, present only while a model is loaded and the window is not fullscreen: **Grid**, **Snap** (axis snap for the truck/pan tool), **Speed** (opens the speed flyout), **Fit**, **Reset**, **Share**, and an overflow **"…"** button — each a 40-logical-pixel icon button (overflow 34px). A draggable empty strip follows, then the centered filename, then another drag strip. On the right, **Open With** (72px, hidden with no model loaded or while fullscreen) precedes the system **Minimize / Maximize-Restore / Close** buttons (46px each), which remain visible even in fullscreen.

The overflow menu is a native popup `HMENU` (the one non-D2D menu surface) containing, conditionally, "Model warnings…" and a separator, then "Controls", a separator, and "About 3D Preview". It does **not** contain cache commands — there is no derived-data cache in this slice — or a recent-files list.

### Bottom bar (44 logical px)

Shown only with a model loaded and not fullscreen: an **Info** toggle at the far left, a **zoom slider** with a live percentage readout on the right, and a **Fullscreen** toggle at the far right.

### Speed flyout

Clicking Speed opens a floating 220 × 64 logical-pixel panel anchored beneath it, with a draggable slider mapping a log scale from 0.05× to 40× fly speed and a live numeric readout. The mouse wheel while fly-looking (RMB held) adjusts the same value.

### Navigation gizmo

A Blender-style six-axis ball gizmo sits in the viewport's top-right corner (44 logical-pixel outer radius) with colored axis stems. Dragging the ball orbits the camera using the same math as an ordinary orbit drag; clicking an axis node snaps to that canonical view.

### Information panel ("Stats & Shading")

Docked to the right edge, a fixed 300 logical px wide (clamped to 90% of viewport width when narrow), scrollable, and modeled on the layout of the discontinued Windows 11 3D Viewer app. Sections: Dimensions (width/height/depth in meters), Mesh Data (triangle/vertex counts, UV0/UV1 presence, vertex colors, material count), Texture Data (per-channel presence — currently always "No"/0 because the GLB slice does not yet decode textures), Animation Data (bone/skin/take counts), Performance Data (draw calls), and Scene Data (node count). Toggled by the bottom-bar Info button or the `ID_VIEW_INFO` command.

### Transient HUDs and tooltips

A speed HUD ("Travel speed ×1.20") and a mode HUD ("Ground grid shown", "Orthographic", "Axis snap on") appear on the corresponding action and fade out: visible 1.3 s, then a 0.30 s fade. Chrome buttons use a custom hover-delay tooltip (1500 ms delay) rather than the stock Win32 tooltip control.

### Color palette

Panel/bar fill #2C2C2E / #242426; borders #3A3A3C / #48484C; primary text #F5F5F7; secondary text #A1A1A6; accent/active #0A84FF; disabled fill/text #2C2C2E / #707075; Close-button hover/press #E81123 / #C42B1C; warning #FF9F0A; error #FF453A. Chrome text uses "Segoe UI Variable Text" semibold.

## Application states

`ViewerState` has four values today: **Empty**, **Loading**, **Ready**, **Failed**. There is no separate Opening/Building-preview/Refining/Pressure/Device-recovery distinction yet — those remain targets in [04-rendering-and-streaming.md](./04-rendering-and-streaming.md) for when progressive multi-format loading lands.

| State | Canvas | Available actions |
| --- | --- | --- |
| Empty | Dashed drop-target rectangle, text "Drop a GLB model here" | Open, drop |
| Loading | Indeterminate D2D spinner; the previous model, if any, stays visible and fully interactive underneath | Camera on old scene, cancel (Esc), replace open |
| Ready | Full model, bottom bar and info panel available | Full interaction |
| Failed | Error card over the prior model or empty background | Retry, Open another, Copy details |

The load itself runs on a single detached worker thread per open (not a pool). Each open increments a generation counter; a new open or window close invalidates in-flight results for the prior generation, and cancellation is a `std::atomic_bool` checked by the load thread.

## Open behavior

Open is available through:

- the initial command-line path, if one was passed to the process;
- Ctrl+O or the (currently overflow-only, chrome-driven) Open action using `IFileOpenDialog`, filtered to `GLB 3D models (*.glb)` plus `All files (*.*)`, with `FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST`;
- drag/drop, implemented as `WM_DROPFILES` via `DragAcceptFiles` (a plain OLE drag-accept, not a custom `IDropTarget`).

Only `.glb` is accepted; every other extension fails immediately with "This format is not included in the current slice. / Other model formats are deliberately deferred." — there is no content sniffing or partial attempt at other formats. A UNC or remote path fails with "Remote model paths are not opened." Dropping more than one file fails with "Open one model at a time." There is no single-instance IPC/mutex forwarding in this slice: a second launch does not activate an existing window, and this document does not yet claim FR-09's later-activation behavior for the GLB slice.

## Camera and input

Camera orientation is a double-precision quaternion with no Euler-angle singularity; render transforms are camera-relative. Pointer mode is an exclusive state machine chosen on button-down:

| Action | Mapping |
| --- | --- |
| Orbit | LMB drag (turntable, pitch-clamped near the poles); cursor wraps at viewport edges during the drag and orbit continues with exponential inertia after release |
| Click-select / clear selection | LMB press+release under 6 px movement and 0.5 s selects a mesh by ray-pick, or clears selection if nothing is hit |
| Pan (truck) | MMB drag along the flattened ground plane, with optional axis snap (Snap button); same wrap+inertia treatment as orbit |
| Dolly (drag) | Ctrl+MMB drag, exponential log-distance mapping, no cursor wrap |
| Fly-look | RMB held: cursor hidden, raw `WM_INPUT` deltas drive look (independent of OS pointer acceleration); W/A/S/D translate, Q/E move down/up, Z/C roll, Shift doubles speed, wheel or the Speed flyout adjusts a persistent 0.05×–40× fly-speed multiplier |
| Orbit via gizmo | Dragging the navigation gizmo ball uses the same orbit math as an LMB drag |
| Orbit/pan by keyboard | Arrow keys orbit normally, or pan while the flight-speed modifier is held |
| Dolly (wheel/pinch) | Mouse wheel, `+`/`-` (or numpad Add/Subtract) for discrete steps, or pinch; wheel/pinch dolly is exponential and clamped to `sceneRadius * [0.025, 250]` |
| Touch | One-finger drag orbits; two-finger drag pans (centroid) and pinches to dolly |
| Fit | F, numpad Decimal, double-click, or the Fit button — frames the current selection if one exists, otherwise the whole model |
| Reset | Home, R, or the Reset button — eases back to the framing captured at load |
| View snaps | Numpad 1/3/7 for Front/Right/Top (Ctrl+ same key for Back/Left/Bottom); Numpad 5 cross-fades Perspective/Orthographic over 0.16 s |
| Toggle ground grid | G or Shift+Alt+G, or the Grid button; guarded against key-repeat for 0.30 s |
| Cancel open / exit fullscreen | Esc |
| Toggle fullscreen | F11 |
| Open overflow menu | Alt+M |
| Open | Ctrl+O |

## Errors

The error card is drawn with Direct2D; only its three action buttons — **Retry**, **Open another**, **Copy details** — are real, tab-stoppable HWND buttons. Current literal messages:

| Condition | Message |
| --- | --- |
| Unsupported extension | "This format is not included in the current slice. / Open a self-contained .glb file. Other model formats are deliberately deferred." |
| Remote/UNC path | "Remote model paths are not opened. / Choose a GLB stored on a local drive for this preview slice." |
| Multiple dropped files | "Open one model at a time. / Drop exactly one local .glb file into the viewer." |
| Out of memory | "There is not enough memory to open this model. / GLB parsing exceeded the available memory budget." |
| Unspecified parse failure | "This GLB could not be previewed. / The importer stopped unexpectedly while reading the model." |
| Open-dialog creation failure | "The Open dialog is unavailable. / Windows could not create the system file picker." |
| Open-dialog runtime failure | "The Open dialog stopped unexpectedly. / Try dropping a local .glb file into the window." |
| Renderer init failure | "Graphics could not be started." |
| Resize failure | "The viewport could not be resized." |
| Upload failure | "The model was read but could not be displayed." |

Copy details always writes `summary\r\n\r\ndetails\r\n\r\nFormat: GLB\r\nPhase: opening` — the format field is currently hardcoded to GLB — and never includes the file path; there is no "Include file path" checkbox because the path is unconditionally omitted rather than optionally included. The Controls command shows a plain `MessageBoxW` listing the full control scheme (the content of the camera table above); About shows a plain `MessageBoxW` ("A focused, native GLB inspection slice. No cloud, no editing, no file modification."). A separate `IDD_ABOUTBOX` dialog template exists in the resource script but is not wired to any command and should be treated as dead until reused or removed.

## Accessibility

Accessibility support is minimal today and this document does not claim more than exists: DPI scaling is real and per-monitor-v2, and the three HWND error-card buttons carry `WS_TABSTOP` and get default Win32 automation for free. There is no UI Automation provider for the D2D-drawn chrome, gizmo, info panel, or canvas; no high-contrast palette swap; no reduce-motion handling for orbit inertia, HUD fades, or the ortho/perspective cross-fade; and no live-region status announcements. [NFR-10](./01-product-scope.md) remains an open gap against this slice, to be closed alongside the accessibility work already scoped in [10-delivery-plan.md](./10-delivery-plan.md) Gate 5.

## Settings and persistence

No setting persists across launches: grid visibility, axis-snap state, and window placement (the latter tracked only in memory to support fullscreen restore) all reset on the next run. There is no registry or settings-file I/O anywhere in this slice, and no derived-data cache — the cache-enabled preference, `%LOCALAPPDATA%` cache directory, and Clear cached previews command described in [04-rendering-and-streaming.md](./04-rendering-and-streaming.md) are forward targets, not current behavior.

## Shell integration

Two on-demand, title-bar-triggered integrations exist, both invoked by the user rather than passive Explorer registration:

- **Open With**: enumerates the current extension's recommended handlers with `SHAssocEnumHandlers` and falls back to `SHOpenWithDialog` for "Choose another app…".
- **Share**: shares the current file as a `StorageFile` through `IDataTransferManagerInterop`/`DataTransferManager`.

No thumbnail provider, no `IInitializeWithStream`/`IThumbnailProvider` implementation, and no installer/COM registration exist in this codebase yet; [05-thumbnail-provider.md](./05-thumbnail-provider.md) and [08-installation-and-registration.md](./08-installation-and-registration.md) describe work not yet started.

## UX acceptance scenarios (current slice)

1. Cold launch shows the #1C1C1E background immediately with no white/flash frame, then the empty-state drop target.
2. Opening a valid .glb replaces the empty state with the loading spinner, then the model; opening a second .glb while the first is still loading keeps the first interactive until the new one completes or fails.
3. Dropping a non-.glb file, a UNC path, or more than one file each produce the corresponding actionable error and leave any currently open model untouched.
4. Every camera action in the table above is reachable by mouse, keyboard, or touch as specified, including fly-look, orbit inertia, and the perspective/orthographic cross-fade.
5. Fullscreen (F11) and Maximize are visibly distinct and independently reversible; Esc exits fullscreen without closing the document.
6. Copy details on any error reproduces exactly the hardcoded `Format: GLB` block above and never includes a file path.
7. Closing the window during an in-progress load leaves no visible window and no lingering process.
8. The information panel's Texture Data section reads all-"No"/0 for any current fixture, since texture decode is not yet implemented in this slice.
