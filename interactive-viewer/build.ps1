<#
.SYNOPSIS
    Deprecated shim. Forwards to .\scripts\build.ps1 at the repository root.
.DESCRIPTION
    This script used to run MSBuild against interactive-viewer\Preview3D.vcxproj
    directly. That is exactly the thing .docs/TODO.md forbids:

        Build Preview3D.slnx, never an individual .vcxproj -- $(SolutionDir) is
        undefined for a project-level build, and 85 of the import-isolation
        tests then fail in a way that looks exactly like a broken sandbox.

    It also wrote its output to interactive-viewer\x64\<Config>\, a second
    output tree sitting beside the real one at <repo>\x64\<Config>\, which is
    what made that failure mode so easy to misdiagnose -- both trees exist on
    disk and it is trivial to inspect the wrong one and conclude the DLLs are
    fine. See .docs/PROGRESS.md, "Building a .vcxproj directly makes 85 of 135
    import-isolation tests fail".

    Kept as a forwarding shim rather than deleted, so an existing habit or
    shortcut lands on the correct build instead of on "file not found".
    Prefer .\scripts\build.ps1 directly.
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Release,
    [switch]$Run
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$configuration = if ($Release) { 'Release' } else { 'Debug' }

Write-Warning "interactive-viewer\build.ps1 is deprecated; use .\scripts\build.ps1 -Configuration $configuration"

$buildScript = Join-Path $repositoryRoot 'scripts\build.ps1'

if ($Clean) {
    & $buildScript -Configuration $configuration -Target Clean
}

& $buildScript -Configuration $configuration

if ($Run) {
    $executablePath = Join-Path $repositoryRoot "x64\$configuration\Preview3D.exe"
    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Built executable was not found at '$executablePath'."
    }

    Write-Host 'Starting Preview3D...'
    Start-Process -FilePath $executablePath -WorkingDirectory $repositoryRoot
}
