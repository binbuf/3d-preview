# Interactive viewer vertical slice

This project is a focused native Windows 11 GLB viewer. It intentionally leaves the thumbnail provider,
file registration, caching, streaming/LOD infrastructure, and broader format support for later slices.

## Included

- GLB 2.0 files with an embedded BIN chunk
- triangle meshes using float `POSITION` and optional float `NORMAL`
- unsigned 8/16/32-bit indices, unindexed triangles, vertex colors, node transforms, and base-color factors
- asynchronous mapped-file import with cancellation and stale-load replacement
- drag/drop, system Open dialog, command-line paths, loading and recoverable error states
- mouse orbit/pan plus right-drag free look and smooth six-degree-of-freedom keyboard flight
- DPI-aware dark native UI, keyboard-reachable controls, tooltips, and a neutral studio/grid render

## Deliberately deferred

Textures, sparse or quantized accessors, Draco/meshopt compression, animation/skins/morphs, `.gltf`
sidecars, every non-GLB format, D3D12 streaming/residency, cache, shell registration, and installer work are
outside this slice. Deferred geometry features report an actionable in-window error instead of rendering an
ambiguous partial result.

## Controls

| Action | Input |
| --- | --- |
| Orbit | Left drag or arrow keys |
| Pan | Middle drag, Shift+left drag, or Shift+arrow keys |
| Free look | Right drag |
| Fly forward / back / strafe | `W` / `S` / `A` / `D` |
| Fly down / up | `Q` / `E` |
| Roll left / right | `Z` / `C` |
| Fly faster | Hold `Shift` |
| Zoom | Wheel, `+`, or `-` |
| Fit | `F` or double-click |
| Reset | `R` |
| Open | `Ctrl+O` |
| Cancel open | `Esc` |
