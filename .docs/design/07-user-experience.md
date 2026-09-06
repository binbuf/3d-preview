# User experience

## Experience principles

The viewer should feel like opening an image: a window appears immediately, input always has a visible response, a recognizable model appears as soon as a safe proxy exists, and detail improves without taking control away from the user.

The product does not pretend that a multi-gigabyte model is instantly complete. It distinguishes Opening, Building preview, and Refining detail, and it stays useful in every phase.

## Window

The first window is centered at 1000 × 720 logical pixels, constrained to the current work area, with a 480 × 360 minimum. The last size/position is not persisted in the MVP. It uses per-monitor-v2 DPI and responds to live monitor moves.

The visual default is:

- client/background #181A1F;
- subtle neutral ground grid/floor only when it helps scale;
- light gray labels and muted blue progress/accent;
- compact 36-logical-pixel top bar;
- model centered with a 7% frame margin;
- no onboarding dialog, project browser, inspector, timeline, or persistent side panels.

To obtain a borderless Preview-like appearance without discarding Windows behavior, the app creates WS_OVERLAPPEDWINDOW and customizes the non-client area with DWM. It preserves resize borders, Snap Layouts, Alt+Space system menu, taskbar behavior, minimize/maximize/restore, keyboard system commands, rounded corners, and correct hit testing. The native client background brush is the same #181A1F, preventing a white flash before the first swap-chain frame.

The top bar contains the filename, Open, Fit, Reset, an overflow menu, Minimize, Maximize/Restore, and Close. The overflow menu contains Enable derived cache, Clear cached previews, and About; it does not contain recent files. Controls visually recede when pointer/focus is elsewhere but remain keyboard reachable. The full canonical path is available as an accessible description and tooltip, not permanently exposed in the title.

High contrast replaces product colors with system colors and disables transparency/fades. Reduce-motion disables camera easing, proxy cross-fades, and spinning animation.

## Application states

| State | Canvas | Top/status surface | Available actions |
| --- | --- | --- | --- |
| Empty | Background and unobtrusive drop target | “Drop a 3D model or open a file” | Open, drop, close |
| Opening | Existing model if present, otherwise background | Filename + “Checking cached preview…” then phase-appropriate activity | Camera on old scene, cancel, replace open |
| Building preview | Partial coarse proxy as safe chunks arrive | “Building preview” and scanned geometry/bytes when known | Full camera interaction |
| Refining | Complete coarse model with progressive detail | Thin progress line + “Refining detail”; resident/detail status in tooltip | Full interaction, fit/reset, cancel refinement |
| Ready | Best detail allowed by view and memory budget | Filename; warnings badge if needed | Full interaction |
| Pressure | Stable proxy/reduced texture mips | Brief “Showing reduced detail—GPU memory is limited” | Full interaction |
| Failed new open | Prior model or empty background | Actionable error card | Retry/Open another/Copy details |
| Device recovery | Last safe background | “Restoring graphics…” | Window/close input; model input resumes on recovery |

Status text never claims 100% until verified bounds and complete coarse proxy are published. Fine-detail residency can be open-ended as the camera moves; its indicator completes when current visible requests are satisfied, not when all source detail is resident.

Progress reporting uses work units appropriate to a phase:

- mapping/metadata: indeterminate;
- compatibility-host startup/composition: indeterminate startup followed by bounded dependency/payload counts when known;
- scan/normalize: validated source bytes or declared primitives, capped below 100%;
- proxy build: completed spatial clusters;
- upload: fence-complete proxy bytes, not merely queued bytes;
- refine: visible requested clusters ready.

Updates are coalesced to at most 10 UI updates per second. This keeps progress useful without flooding the message queue or layout path.

The cache-enabled preference is the only persisted viewer setting in MVP and is stored in a small versioned current-user settings file, never in the file-association registry. Clearing cached previews may show one coalesced progress/status update, remains cancellable, and does not block camera input or close.

## Open behavior

Open is available through:

- initial Explorer activation/default-app invocation;
- Ctrl+O and the Open button using IFileOpenDialog;
- drag/drop through an OLE IDropTarget;
- a later authenticated activation from another viewer launch.

The dialog filters all direct extensions and also offers All supported 3D models. .mtl is not offered because it is a sidecar. A drop containing exactly one supported local file opens it. Multiple items show “Open one model at a time”; directories and remote/device items are rejected without probing their contents.

When replacing a visible document, the current model stays interactive until the new complete coarse proxy is safe. Success changes the title and view atomically. Failure keeps the old scene and camera.

## Camera and input

The default camera is perspective with vertical field of view 45 degrees. Up follows the source convention converted by its adapter. Fit frames verified finite bounds; Reset returns to the adapter's canonical up/front orientation and then fits.

| Action | Mouse, keyboard, and touch |
| --- | --- |
| Orbit | Left drag; canvas-focused arrow keys; one-finger touch drag |
| Pan | Middle drag, Shift+left drag, or Shift+arrow keys; two-finger touch drag |
| Dolly/zoom | Wheel/precision scroll, `+`/`-`, or pinch |
| Fit | F or Fit button; double-click canvas |
| Reset orientation + fit | R or Reset button |
| Open | Ctrl+O |
| Open overflow menu | Alt+M |
| Clear cached previews | overflow command with confirmation; accessible accelerator shown in menu |
| Cancel current open/refinement | Esc |
| System menu | Alt+Space |
| Close | Alt+F4 |

Orbit is a trackball around the current target with no Euler-angle singularity. Pan scale derives from depth and viewport height. Wheel/pinch movement is accumulated at high resolution and applies an exponential dolly, so notched mice, precision touchpads, and touchscreens behave consistently. Keyboard camera steps repeat smoothly but remain frame-rate independent. Camera distance and target use double precision; render transforms are camera-relative.

Input events are timestamped and consumed on the next render frame. Pointer capture continues drags beyond the client edge and is released on button-up, capture loss, deactivation, or close. Resizing, DPI change, and loading do not reset the camera.

If provisional bounds are later corrected, automatic camera easing occurs only when the user has not interacted. Once they have, the app preserves orientation and apparent target, adjusting numerical scale clamps without a surprise reframe.

## Progressive visual behavior

Partial proxy geometry uses normal neutral shading, not a flashing debug wireframe. Regions not yet represented may show a restrained animated bounding-box/grid treatment. Newly available spatial regions fade in over 100 ms unless reduce-motion is active. Parent LOD stays drawn until its replacement is fence-complete, preventing holes and sparkling.

When camera motion outpaces streaming, detail may step down immediately to maintain frame rate. Refinement resumes after approximately 150 ms of stable view. No loading spinner runs as an independent high-frequency UI animation; it is advanced by presented frames and stops when the window is occluded/minimized.

Transparent materials, missing textures, unsupported optional features, and dropped invalid primitives yield a nonmodal warning badge. Selecting the badge opens a compact diagnostics flyout grouped by category; it never exposes thousands of repeated per-triangle warnings.

## Errors

Error cards contain:

- a plain-language summary;
- a useful next action;
- the format and failed phase;
- Retry, Open another, and Copy details actions;
- an expandable technical code/correlation ID.

Examples:

| Condition | Message |
| --- | --- |
| Unsupported required feature | “This glTF requires skinning, which this static preview does not support.” |
| Compressed primitive limit | “A Draco-compressed mesh expands beyond this version’s safe per-mesh limit.” |
| Limit | “This FBX has more than the supported 20 million triangles.” |
| Sidecar missing | “The model opened, but materials.bin was not found. Geometry is shown with default materials.” |
| Unsafe reference | “A model resource points outside its folder and was not opened.” |
| GPU pressure | “The model is open at reduced detail because graphics memory is limited.” |
| Invalid file | “This file is damaged or is not a valid 3MF model.” |
| Compatibility host failure | “The USD compatibility importer stopped while opening this stage. The viewer is still usable.” |

Errors do not offer to search online or upload the model. Copy details omits the full path unless the user explicitly checks Include file path.

## Accessibility

Every top-bar button, overflow item, and error action exposes UI Automation role, name, help text, enabled/state, invoke or toggle pattern, and focus rectangle. Status changes use polite live-region announcements at phase transitions only, not per progress tick. The canvas exposes the document name and state as an accessible pane; direct manipulation of 3D geometry has no semantic selection in the MVP.

Keyboard focus order is Open, Fit, Reset, diagnostics/status actions, window controls, then canvas. Focus is never stolen when a proxy/detail chunk appears. At 200% scale, controls retain at least 32 × 32 logical-pixel hit targets and error/status text reflows without clipping. Windows text scale is honored separately from DPI.

DirectWrite performs text layout and Direct2D draws overlays through the render-thread bridge defined in [04-rendering-and-streaming.md](./04-rendering-and-streaming.md). UI Automation objects live with the UI thread and read immutable, compact AppState snapshots; they never inspect renderer data or wait for a frame.

## Responsiveness rules

- UI event handlers target less than 2 ms and may only enqueue bounded work.
- A camera event should affect the next eligible frame and meet the 16 ms p95 input-to-present target.
- Progress rendering must not allocate per frame after steady state.
- Opening dialogs, system menus, accessibility queries, and window dragging remain usable during a multi-gigabyte scan.
- The loading UI degrades by reducing animated detail before it misses frame-time targets.
- The title never appends “Not responding” because of product-owned synchronous work; automated tests fail if the UI message heartbeat misses 100 ms during load.

## UX acceptance scenarios

1. Cold launch on a 144 Hz display shows #181A1F immediately with no white frame, then a responsive empty state.
2. Opening a 4 GiB Tier A fixture allows continuous orbit/resize while proxy and detail arrive.
3. Opening a corrupt model while another is visible preserves the old model and produces an actionable error.
4. Dragging a valid OBJ next to MTL/textures uses them; an Explorer OBJ thumbnail remains safe without them.
5. Reducing the GPU budget transitions to a stable lower LOD with a single nonblocking notice.
6. Keyboard-only and 200% DPI/high-contrast runs can invoke every command and read every state.
7. Closing during each loading phase hides promptly and leaves no tray icon or background process.
8. A verified warm-cache reopen publishes the cached coarse proxy within its performance gate; a stale/corrupt entry silently falls back to an ordinary open.
9. Keyboard-only and touch runs can orbit, pan, zoom, fit, and reset; clearing the cache removes derived entries without affecting models or Explorer thumbnails.
10. A USD stage that exceeds the TinyUSDZ subset starts the AppContainer compatibility path without freezing the old scene; host failure leaves the viewer responsive.
