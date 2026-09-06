The architecture for a Windows 11 3D model quick-preview utility requires a hybrid approach, separating the OS-level shell integration from the standalone rendering application to maximize stability and performance for heavy CAD or 3D printing assets.

## System Architecture

The solution is divided into two distinct artifacts: an out-of-process COM DLL for Windows Explorer integration and a Flutter-based native executable for the interactive viewer.

### 1. Windows Shell Extension (Thumbnail Provider)

* **Tech Stack:** Visual Studio C++ (Native)
* **Target Interfaces:** `IThumbnailProvider`, `IInitializeWithStream`
* **Responsibilities:**
* Accept binary streams from the Windows shell for `.glb`, `.gltf`, and `.stl` files.
* Parse the geometry using a lightweight C++ library (e.g., Assimp).
* Render a single frame to an off-screen buffer and return an `HBITMAP` to File Explorer.


* **Design Constraints:** Must be purely native C++ with explicit memory management (`AddRef`/`Release`). Running within the `dllhost.exe` surrogate process requires strict crash isolation to prevent silently breaking the Windows thumbnail cache.

### 2. Interactive Viewer ("Preview" App)

* **Tech Stack:** Flutter Desktop (Dart) with modified C++ Windows Runner.
* **Rendering Engine:** Impeller (via direct Direct3D 12/Vulkan access) with `flutter_scene`.
* **Responsibilities:**
* Act as the default registered application for 3D file extensions.
* Provide a borderless, minimalist, macOS-style window for instant model rotation and inspection.
* Handle massive polygon counts by bypassing browser memory limits (V8/Chromium) and leveraging Dart FFI for zero-copy memory transfers directly to the GPU.



## Performance & Lifecycle Optimizations

To replicate the instantaneous feel of a native OS utility while handling heavy assets, the Flutter application relies on three specific startup optimizations.

### Configurable Daemon Mode (Fast Boot)

To eliminate the Dart VM cold-boot penalty, the application includes a user-configurable background execution setting.

* **Enabled (Default for speed):** Overrides the `WM_CLOSE` message in the Win32 C++ runner. Instead of terminating the process, it calls `ShowWindow(SW_HIDE)`. A lightweight launcher (or named pipe listener) intercepts double-clicks in Explorer and sends the new file path to the warm, hidden Flutter process, rendering the window in ~16ms. A system tray icon allows the user to fully exit.
* **Disabled (Opt-in for resource constraints):** The application terminates completely on close, respecting users who prefer zero background footprint, at the cost of a standard Flutter cold-start delay (~300-500ms).

### Asynchronous File Parsing

The main Dart thread handles only UI layout and window initialization. All binary parsing of the `.glb` or `.stl` files is offloaded to a separate Dart Isolate using `compute()`. The UI instantly displays a matching Win32 background color and a loading state until the isolate passes the geometry pointer to the Impeller engine.

### Native Splash Matching

The Win32 `WNDCLASS` background brush in the C++ runner is hardcoded to match the Flutter application's exact background hex code. When Windows executes the binary, it draws a solid color instead of a jarring white flash before the Flutter engine paints its first frame.
