# Third-party notices

One file per third-party component, containing that project's licence text
copied verbatim from the licence it ships (via the `copyright` file vcpkg
installs alongside each package).

These are reproduced, not summarised. Do not edit them to shorten or
reformat -- Apache-2.0 section 4 and the BSD/MIT reproduction clauses require
the notice to travel with the distribution intact.

The index, including which components are actually linked today and which are
pinned but not yet wired, is in [`../../NOTICE`](../../NOTICE).

To refresh after a dependency version change:

```powershell
$share = 'vcpkg_installed\x64-windows\x64-windows\share'
foreach ($p in 'catch2','fastgltf','draco','basisu','ktx','meshoptimizer','directxtex','libwebp') {
    Copy-Item "$share\$p\copyright" "third_party\notices\$p.txt" -Force
}
```

Then update `NOTICE` with the new version numbers in the same commit.
