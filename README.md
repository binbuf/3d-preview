# 3d-previewer

A tiny, optimized Windows utility to view popular 3D models formats

## Getting Started

TODO

## Building and testing

Requires Visual Studio 2026 with the Desktop development with C++ workload
(the `v145` platform toolset pinned in `Directory.Build.props`).

```powershell
.\scripts\build.ps1 -Configuration Both
.\scripts\run-tests.ps1
```

Always build `Preview3D.slnx`, never an individual `.vcxproj` -- `$(SolutionDir)`
is undefined for a project-level build, and the import-isolation suite then
fails in a way that looks exactly like a broken sandbox. The scripts above take
care of this.

## Licence

Apache License 2.0 -- see [LICENSE](LICENSE).

Third-party components and their licences are indexed in [NOTICE](NOTICE), with
verbatim licence texts under [`third_party/notices/`](third_party/notices/).

## Credit

<a href="https://www.flaticon.com/free-icons/geometric" title="geometric icons">Geometric icons created by Magnific - Flaticon</a>

<a href="https://www.flaticon.com/free-icons/perspective" title="perspective icons">Perspective icons created by Iconir - Flaticon</a>

<a href="https://www.flaticon.com/free-icons/grid" title="grid icons">Grid icons created by Magnific - Flaticon</a>

<a href="https://www.flaticon.com/free-icons/speed" title="speed icons">Speed icons created by Magnific - Flaticon</a>
